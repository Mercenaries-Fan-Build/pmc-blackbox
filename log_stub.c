/**
 * log_stub.c — see log_stub.h.
 */
#include "log_stub.h"

#include <stdio.h>
#include <string.h>

/* At most this many "print" strings are tracked; the retail images hold one or two. */
#define MAX_PRINT_STRINGS 16

static uint32_t u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int in_section(const LogStubSection *s, uint32_t va) {
    return va >= s->va && va - s->va < s->size;
}

int FindLogStub(const LogStubSection *text, const LogStubSection *rdata,
                uint32_t *stub_va, uint32_t *row_va, char *why, size_t why_len) {
    static const uint8_t needle[] = { 0, 'p', 'r', 'i', 'n', 't', 0 };
    uint32_t strings[MAX_PRINT_STRINGS];
    int nstrings = 0;
    uint32_t found_row = 0, found_fn = 0;
    int rows = 0;
    uint32_t i, at;
    const uint8_t *head;

    if (why_len > 0) why[0] = '\0';
    if (!text || !rdata || !text->bytes || !rdata->bytes || rdata->size < sizeof(needle)) {
        snprintf(why, why_len, "the image has no readable .text or .rdata");
        return 0;
    }

    for (i = 0; i + sizeof(needle) <= rdata->size; i++) {
        if (memcmp(rdata->bytes + i, needle, sizeof(needle)) == 0) {
            if (nstrings == MAX_PRINT_STRINGS) {
                snprintf(why, why_len, "more than %d \"print\" strings in .rdata", MAX_PRINT_STRINGS);
                return 0;
            }
            strings[nstrings++] = rdata->va + i + 1;
        }
    }
    if (nstrings == 0) {
        snprintf(why, why_len, "no \"print\" string in .rdata");
        return 0;
    }

    for (at = 0; at + 8 <= rdata->size; at += 4) {
        uint32_t p = u32_le(rdata->bytes + at);
        uint32_t f;
        int k, named = 0;
        for (k = 0; k < nstrings; k++) {
            if (strings[k] == p) { named = 1; break; }
        }
        if (!named) continue;
        f = u32_le(rdata->bytes + at + 4);
        if (!in_section(text, f)) continue;
        rows++;
        found_row = rdata->va + at;
        found_fn = f;
    }
    if (rows != 1) {
        snprintf(why, why_len, "%d luaL_Reg rows named \"print\" point into .text; exactly one is required", rows);
        return 0;
    }

    if (found_fn - text->va + 3 > text->size) {
        snprintf(why, why_len, "print at 0x%08X runs past the end of .text", (unsigned)found_fn);
        return 0;
    }
    head = text->bytes + (found_fn - text->va);
    if (!(head[0] == 0x33 && head[1] == 0xC0 && head[2] == 0xC3) && head[0] != 0xE9) {
        snprintf(why, why_len,
                 "print at 0x%08X (row 0x%08X) starts %02X %02X %02X, neither the log stub 33 C0 C3 "
                 "nor a detour's E9 jump",
                 (unsigned)found_fn, (unsigned)found_row, head[0], head[1], head[2]);
        return 0;
    }

    *stub_va = found_fn;
    *row_va = found_row;
    return 1;
}
