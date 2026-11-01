/**
 * sites.c — see sites.h.
 */
#include "sites.h"

#include <stdio.h>
#include <string.h>

/* The longest pattern in the table, in bytes. */
#define MAX_PATTERN 128

typedef struct {
    uint8_t  byte[MAX_PATTERN];
    uint8_t  fixed[MAX_PATTERN];
    uint32_t len;
} Pattern;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "8b ?? c3" into bytes and a fixed-byte mask. The table is generated, so a bad one is a bad header. */
static int parse(const char *text, Pattern *p) {
    p->len = 0;
    while (*text) {
        if (p->len == MAX_PATTERN) return 0;
        if (text[0] == '?' && text[1] == '?') {
            p->byte[p->len] = 0;
            p->fixed[p->len] = 0;
        } else {
            int hi = hex_digit(text[0]), lo = hi < 0 ? -1 : hex_digit(text[1]);
            if (lo < 0) return 0;
            p->byte[p->len] = (uint8_t)(hi * 16 + lo);
            p->fixed[p->len] = 1;
        }
        p->len++;
        text += 2;
        if (*text == ' ') text++;
    }
    return p->len > 0 && p->fixed[0];
}

/* How many times `p` matches, with the last match's offset in `at`. */
static uint32_t count_matches(const Pattern *p, const uint8_t *hay, uint32_t size, uint32_t *at) {
    uint32_t n = 0, i = 0, last;
    if (size < p->len) return 0;
    last = size - p->len;
    while (i <= last) {
        const uint8_t *hit = memchr(hay + i, p->byte[0], last - i + 1);
        uint32_t k;
        if (!hit) break;
        i = (uint32_t)(hit - hay);
        for (k = 1; k < p->len; k++)
            if (p->fixed[k] && hay[i + k] != p->byte[k]) break;
        if (k == p->len) { n++; *at = i; }
        i++;
    }
    return n;
}

int PmcSitesResolve(const uint8_t *text, uint32_t text_va, uint32_t text_size,
                    uint32_t values[PMC_SITE_COUNT], char *why, size_t why_len) {
    uint32_t found[PMC_SITE_COUNT], offset[PMC_SITE_COUNT];
    int i, ok = 1;
    size_t off = 0;

    for (i = 0; i < PMC_SITE_COUNT; i++) {
        Pattern p;
        if (!parse(PMC_SITE_DEFS[i].pattern, &p)) {
            snprintf(why, why_len, "the pattern of %s does not parse", PMC_SITE_DEFS[i].name);
            return 0;
        }
        offset[i] = 0;
        found[i] = count_matches(&p, text, text_size, &offset[i]);
        if (found[i] != 1) ok = 0;
    }
    if (!ok) {
        off = (size_t)snprintf(why, why_len, "sites did not resolve to exactly one match each:");
        for (i = 0; i < PMC_SITE_COUNT && off < why_len; i++)
            off += (size_t)snprintf(why + off, why_len - off, " %s=%u", PMC_SITE_DEFS[i].name, (unsigned)found[i]);
        return 0;
    }

    for (i = 0; i < PMC_SITE_COUNT; i++) {
        if (PMC_SITE_DEFS[i].operand_at < 0) {
            values[i] = text_va + offset[i];
        } else {
            const uint8_t *b = text + offset[i] + PMC_SITE_DEFS[i].operand_at;
            values[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
    }
    return 1;
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern void pmc_log(const char *source, const char *fmt, ...);

/* How long to wait for SecuROM to decrypt .text, and how often to look. */
#define SITES_WAIT_MS  30000
#define SITES_RETRY_MS 50

static uint32_t      g_values[PMC_SITE_COUNT];
static volatile LONG g_resolved;

uint32_t PmcSite(int site) {
    return g_resolved ? g_values[site] : 0;
}

int PmcSiteHolds(int site, uint32_t eip) {
    uint32_t start = PmcSite(site);
    return start != 0 && eip >= start && eip - start < PMC_SITE_DEFS[site].function_len;
}

/* The mapped .text, over the span that exists in both the file and the image. */
static int mapped_text(const uint8_t **bytes, uint32_t *va, uint32_t *size) {
    const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(s[i].Name, ".text\0\0\0", 8) != 0) continue;
        *bytes = base + s[i].VirtualAddress;
        *va = (uint32_t)(base + s[i].VirtualAddress);
        *size = s[i].Misc.VirtualSize < s[i].SizeOfRawData ? s[i].Misc.VirtualSize : s[i].SizeOfRawData;
        return 1;
    }
    return 0;
}

/* Every page of the span committed and readable. */
static int readable(const uint8_t *bytes, uint32_t size) {
    const uint8_t *p = bytes, *end = bytes + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) || (mbi.Protect & PAGE_GUARD))
            return 0;
        p = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int try_resolve(char *why, size_t why_len) {
    const uint8_t *bytes;
    uint32_t va, size;
    int i;

    if (!mapped_text(&bytes, &va, &size)) {
        snprintf(why, why_len, "the image has no .text section");
        return 0;
    }
    if (!readable(bytes, size)) {
        snprintf(why, why_len, ".text is not readable");
        return 0;
    }
    if (!PmcSitesResolve(bytes, va, size, g_values, why, why_len)) return 0;

    InterlockedExchange(&g_resolved, 1);
    for (i = 0; i < PMC_SITE_COUNT; i++)
        pmc_log("blackbox", "SITE %s=%08lX", PMC_SITE_DEFS[i].name, (unsigned long)g_values[i]);
    return 1;
}

static DWORD WINAPI RetryThread(LPVOID param) {
    char why[512];
    DWORD start = GetTickCount();
    (void)param;
    for (;;) {
        if (try_resolve(why, sizeof(why))) return 0;
        if (GetTickCount() - start >= SITES_WAIT_MS) break;
        Sleep(SITES_RETRY_MS);
    }
    pmc_log("blackbox", "SITES unresolved after %d s, so nothing build-specific runs: %s", SITES_WAIT_MS / 1000, why);
    return 0;
}

void PmcSitesStart(void) {
    char why[512];
    if (!try_resolve(why, sizeof(why)))
        CreateThread(NULL, 0, RetryThread, NULL, 0, NULL);
}

#endif /* _WIN32 */
