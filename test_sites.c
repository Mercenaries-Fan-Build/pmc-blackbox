/**
 * test_sites.c — host test of PmcSitesResolve.
 *
 * Always runs synthetic cases built from the generated table. Given executables as arguments, also
 * reads each one's .text from disk and prints every site, and with `<exe>=<site>:<hex>,...` checks the
 * values named. `make check-sites` passes the reference builds.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "sites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* Lay out one match of every pattern with its wildcards set to `fill`. Returns the length written. */
static uint32_t lay_out(uint8_t *text, uint32_t cap, uint8_t fill, uint32_t at[PMC_SITE_COUNT]) {
    uint32_t n = 0;
    int i;
    memset(text, 0xCC, cap);
    n = 8;
    for (i = 0; i < PMC_SITE_COUNT; i++) {
        const char *p = PMC_SITE_DEFS[i].pattern;
        at[i] = n;
        while (*p) {
            if (p[0] == '?') {
                text[n++] = fill;
            } else {
                unsigned v;
                sscanf(p, "%2x", &v);
                text[n++] = (uint8_t)v;
            }
            p += 2;
            if (*p == ' ') p++;
        }
        n += 8;
    }
    return n;
}

static void synthetic(void) {
    static uint8_t text[8192];
    uint32_t at[PMC_SITE_COUNT], values[PMC_SITE_COUNT];
    char why[1024];
    uint32_t n = lay_out(text, sizeof(text), 0x11, at);
    int i, ok = 1;

    check(PmcSitesResolve(text, 0x401000, n, values, why, sizeof(why)) == 1, "one match of every site resolves");
    for (i = 0; i < PMC_SITE_COUNT; i++) {
        uint32_t want = PMC_SITE_DEFS[i].operand_at < 0 ? 0x401000 + at[i] : 0x11111111;
        if (values[i] != want) ok = 0;
    }
    check(ok, "operands are read and functions give their address");

    memcpy(text + n, text, n);
    check(PmcSitesResolve(text, 0x401000, n * 2, values, why, sizeof(why)) == 0 &&
          strstr(why, "=2") != NULL, "two matches of a site are refused, with the count");

    memset(text, 0xCC, sizeof(text));
    check(PmcSitesResolve(text, 0x401000, 1024, values, why, sizeof(why)) == 0 &&
          strstr(why, "=0") != NULL, "no match is refused, with the count");
    check(PmcSitesResolve(text, 0x401000, 4, values, why, sizeof(why)) == 0, "a section too short for a pattern is refused");
}

static uint32_t u32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Find .text in an exe file; returns 0 when there is none. */
static int file_text(const uint8_t *f, size_t len, const uint8_t **bytes, uint32_t *va, uint32_t *size) {
    uint32_t pe = u32_at(f + 0x3C), base, table;
    unsigned nsec, optsize, i;
    if (pe + 0x18 > len || memcmp(f + pe, "PE\0\0", 4) != 0) return 0;
    nsec = (unsigned)f[pe + 6] | ((unsigned)f[pe + 7] << 8);
    optsize = (unsigned)f[pe + 20] | ((unsigned)f[pe + 21] << 8);
    base = u32_at(f + pe + 24 + 28);
    table = pe + 24 + optsize;
    for (i = 0; i < nsec; i++) {
        const uint8_t *h = f + table + i * 40;
        if (memcmp(h, ".text\0\0\0", 8) != 0) continue;
        *va = base + u32_at(h + 12);
        *size = u32_at(h + 8) < u32_at(h + 16) ? u32_at(h + 8) : u32_at(h + 16);
        *bytes = f + u32_at(h + 20);
        return 1;
    }
    return 0;
}

/* `spec`: the exe's path, then optionally =site:hex,site:hex. */
static void real_exe(char *spec) {
    char *expect = strchr(spec, '=');
    FILE *fp;
    long len;
    uint8_t *f;
    const uint8_t *bytes;
    uint32_t va, size, values[PMC_SITE_COUNT];
    char why[1024], label[600];
    int i;

    if (expect) *expect++ = 0;
    fp = fopen(spec, "rb");
    if (!fp) { snprintf(label, sizeof(label), "open %s", spec); check(0, label); return; }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f = malloc((size_t)len);
    if (!f || fread(f, 1, (size_t)len, fp) != (size_t)len) { check(0, "read the exe"); fclose(fp); return; }
    fclose(fp);

    snprintf(label, sizeof(label), "%s has a .text", spec);
    if (!file_text(f, (size_t)len, &bytes, &va, &size)) { check(0, label); free(f); return; }
    check(1, label);

    if (!PmcSitesResolve(bytes, va, size, values, why, sizeof(why))) {
        snprintf(label, sizeof(label), "%s: %s", spec, why);
        check(0, label);
        free(f);
        return;
    }
    snprintf(label, sizeof(label), "%s resolves every site", spec);
    check(1, label);
    for (i = 0; i < PMC_SITE_COUNT; i++)
        printf("      %-16s %08X\n", PMC_SITE_DEFS[i].name, (unsigned)values[i]);

    while (expect && *expect) {
        char *comma = strchr(expect, ',');
        char *colon = strchr(expect, ':');
        if (comma) *comma = 0;
        if (colon) {
            *colon = 0;
            for (i = 0; i < PMC_SITE_COUNT; i++) {
                if (strcmp(PMC_SITE_DEFS[i].name, expect) == 0) {
                    snprintf(label, sizeof(label), "%s %s = %s", spec, expect, colon + 1);
                    check(values[i] == (uint32_t)strtoul(colon + 1, NULL, 16), label);
                }
            }
        }
        expect = comma ? comma + 1 : NULL;
    }
    free(f);
}

int main(int argc, char **argv) {
    int i;
    synthetic();
    for (i = 1; i < argc; i++) real_exe(argv[i]);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
