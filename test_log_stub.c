/**
 * test_log_stub.c — host test of FindLogStub.
 *
 * Always runs synthetic cases (one row found; none; two; wrong bytes; an existing detour). Given
 * arguments `<exe> <expected-va>` pairs, also reads each exe's .text and .rdata from disk and checks
 * the stub it finds — run by `make check-log-stub EXES="<exe> 0x6AEF90 <exe> 0x6D5640"`.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "log_stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A tiny image: .text at 0x401000, .rdata at 0x500000 holding "\0print\0" at 0x500010 and rows. */
static void synthetic(void) {
    uint8_t text[64], rdata[128];
    LogStubSection t = { 0x401000, text, sizeof(text) };
    LogStubSection r = { 0x500000, rdata, sizeof(rdata) };
    uint32_t stub = 0, row = 0;
    char why[256];

    memset(text, 0xCC, sizeof(text));
    text[0x20] = 0x33; text[0x21] = 0xC0; text[0x22] = 0xC3;
    memset(rdata, 0, sizeof(rdata));
    memcpy(rdata + 0x10, "\0print\0", 7);
    put32(rdata + 0x40, 0x500011);
    put32(rdata + 0x44, 0x401020);

    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 1 && stub == 0x401020 && row == 0x500040,
          "one print row pointing at 33 C0 C3 is found");

    text[0x20] = 0xE9;
    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 1 && stub == 0x401020,
          "a print stub already detoured (E9) is accepted");

    text[0x20] = 0x55;
    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 0 && strstr(why, "neither the log stub"),
          "a print function that is not the stub is refused");
    text[0x20] = 0x33;

    put32(rdata + 0x48, 0x500011);
    put32(rdata + 0x4C, 0x401020);
    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 0 && strstr(why, "2 luaL_Reg rows"),
          "two print rows are refused");
    put32(rdata + 0x48, 0);
    put32(rdata + 0x4C, 0);

    put32(rdata + 0x44, 0x7FFFFF00);
    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 0 && strstr(why, "0 luaL_Reg rows"),
          "a print row whose function is outside .text is not a row");
    put32(rdata + 0x44, 0x401020);

    rdata[0x10] = 'x';
    check(FindLogStub(&t, &r, &stub, &row, why, sizeof(why)) == 0 && strstr(why, "no \"print\" string"),
          "\"print\" without a preceding NUL is not the name");
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Map section `name` of the PE in `file` into a fresh buffer at its virtual address. */
static int load_section(const uint8_t *file, size_t flen, const char *name, LogStubSection *out) {
    uint32_t pe, nsec, opt, image_base, i;
    if (flen < 0x40 || file[0] != 'M' || file[1] != 'Z') return 0;
    pe = le32(file + 0x3C);
    if (pe + 24 > flen || memcmp(file + pe, "PE\0\0", 4) != 0) return 0;
    nsec = le16(file + pe + 6);
    opt = le16(file + pe + 20);
    image_base = le32(file + pe + 24 + 28);
    for (i = 0; i < nsec; i++) {
        const uint8_t *s = file + pe + 24 + opt + i * 40;
        uint32_t vsize, rva, rsize, roff, n;
        uint8_t *buf;
        if (strncmp((const char *)s, name, 8) != 0) continue;
        vsize = le32(s + 8); rva = le32(s + 12); rsize = le32(s + 16); roff = le32(s + 20);
        if (vsize == 0) vsize = rsize;
        buf = calloc(vsize, 1);
        if (!buf) return 0;
        n = rsize < vsize ? rsize : vsize;
        if (roff + n > flen) n = roff < flen ? (uint32_t)(flen - roff) : 0;
        memcpy(buf, file + roff, n);
        out->va = image_base + rva;
        out->bytes = buf;
        out->size = vsize;
        return 1;
    }
    return 0;
}

static void real_exe(const char *path, uint32_t expected) {
    FILE *f = fopen(path, "rb");
    uint8_t *file;
    long flen;
    LogStubSection t, r;
    uint32_t stub = 0, row = 0;
    char why[256], what[1024];
    int ok;

    if (!f) { snprintf(what, sizeof(what), "%s: cannot open", path); check(0, what); return; }
    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)flen);
    if (!file || fread(file, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        snprintf(what, sizeof(what), "%s: cannot read", path);
        check(0, what);
        return;
    }
    fclose(f);
    if (!load_section(file, (size_t)flen, ".text", &t) || !load_section(file, (size_t)flen, ".rdata", &r)) {
        snprintf(what, sizeof(what), "%s: no .text or .rdata section", path);
        check(0, what);
        free(file);
        return;
    }
    ok = FindLogStub(&t, &r, &stub, &row, why, sizeof(why));
    if (ok)
        snprintf(what, sizeof(what), "%s: stub 0x%08X (row 0x%08X), expected 0x%08X",
                 path, (unsigned)stub, (unsigned)row, (unsigned)expected);
    else
        snprintf(what, sizeof(what), "%s: not found: %s", path, why);
    check(ok && stub == expected, what);
    free((void *)t.bytes);
    free((void *)r.bytes);
    free(file);
}

int main(int argc, char **argv) {
    int i;
    synthetic();
    if ((argc - 1) % 2 != 0) {
        fprintf(stderr, "usage: %s [<exe> <expected-va> ...]\n", argv[0]);
        return 2;
    }
    for (i = 1; i + 1 < argc; i += 2)
        real_exe(argv[i], (uint32_t)strtoul(argv[i + 1], NULL, 0));
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
