/**
 * test_pool_stats.c — host test of PmcPoolWalk.
 *
 * Always runs on a small synthetic pool. Given `<process.dmp> <texture_free hex> <texture_in_use hex>`,
 * also walks the pool in a captured minidump and prints it. `make check-pool` passes the v1.1 dumps.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "pool_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmc_sites.gen.h"

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* --- a flat fake address space starting at VA 0x1000 ------------------------------------------- */

#define FAKE_BASE 0x1000u
#define FAKE_SIZE 0x8000u
static uint8_t g_fake[FAKE_SIZE];

static int fake_read(void *ctx, uint32_t va, void *out, uint32_t n) {
    (void)ctx;
    if (va < FAKE_BASE || va - FAKE_BASE + n > FAKE_SIZE) return 0;
    memcpy(out, g_fake + (va - FAKE_BASE), n);
    return 1;
}

static void poke(uint32_t va, uint32_t v) {
    memcpy(g_fake + (va - FAKE_BASE), &v, 4);
}

static void node(uint32_t va, uint32_t lo, uint32_t hi, uint32_t next) {
    poke(va + 0, lo);
    poke(va + 4, hi);
    poke(va + 0x10, next);
}

static void synthetic(void) {
    enum { FREE_GLOBAL = 0x1000, IN_USE_GLOBAL = 0x1010, TEX_TABLE = 0x1100, WIDTHS = 0x1500,
           VB_TABLE = 0x2000, NODES = 0x4000 };
    PmcPoolStats s;

    memset(g_fake, 0, sizeof(g_fake));
    poke(FREE_GLOBAL, TEX_TABLE);
    poke(TEX_TABLE + 4, WIDTHS);
    poke(WIDTHS + 11 * 4, NODES);
    /* two free 2048x2048 DXT1 with 10 levels, the request the recorded crash made */
    node(NODES, 0x0100A0BB, 0x31545844, NODES + 0x18);
    node(NODES + 0x18, 0x0100A0BB, 0x31545844, 0);
    /* a free vertex buffer of 5 units; its list runs off into memory that cannot be read */
    poke(IN_USE_GLOBAL + PMC_POOL_VB_FREE_OFF, VB_TABLE);
    poke(VB_TABLE + 8, NODES + 0x30);
    node(NODES + 0x30, 5 | 0x10000000, 0, 0x7FFFFFF0);

    PmcPoolWalk(fake_read, NULL, FREE_GLOBAL, IN_USE_GLOBAL, &s);
    check(s.texture_free.nodes == 2, "two free textures are counted");
    check(s.texture_free.bytes == 2ull * 2796200, "the 2048x2048 DXT1 size matches the crash's 2,796,200 B");
    check(s.vb_free.nodes == 1 && s.vb_free.bytes == 5 * 2048, "a buffer's size is its low key bits times the unit");
    check(s.vb_free.unreadable == 1, "a list running into unreadable memory is reported, and the walk stops");
    check(s.texture_in_use.nodes == 0 && s.ib_free.nodes == 0, "empty lists stay empty");

    node(NODES, 0x0100A0BB, 0x12345678, 0);
    node(NODES + 0x18, 0, 0, 0);
    PmcPoolWalk(fake_read, NULL, FREE_GLOBAL, IN_USE_GLOBAL, &s);
    check(s.unknown_formats == 1, "an unknown format is counted, not dropped");

    node(NODES, 0x0100A0BB, 0x31545844, NODES);
    PmcPoolWalk(fake_read, NULL, FREE_GLOBAL, IN_USE_GLOBAL, &s);
    check(s.texture_free.nodes == 200000, "a list that loops stops at the bound");

    check(PmcTextureLevelBytes(0x35545844, 4, 4) == 16 && PmcTextureLevelBytes(50, 8, 2) == 16 &&
          PmcTextureLevelBytes(21, 2, 2) == 16 && PmcTextureLevelBytes(0x31545844, 1, 1) == 8, "level sizes");
}

/* --- a minidump's Memory64List ------------------------------------------------------------------ */

typedef struct { uint64_t start, size, file; } Range;
typedef struct { const uint8_t *f; size_t len; Range *r; uint32_t n; } Dump;

static uint32_t u32_at(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t u64_at(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static int dump_read(void *ctx, uint32_t va, void *out, uint32_t n) {
    const Dump *d = ctx;
    uint32_t lo = 0, hi = d->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const Range *r = &d->r[mid];
        if (va < r->start) hi = mid;
        else if (va >= r->start + r->size) lo = mid + 1;
        else {
            if (va + (uint64_t)n > r->start + r->size) return 0;
            memcpy(out, d->f + r->file + (va - r->start), n);
            return 1;
        }
    }
    return 0;
}

static int by_start(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void real_dump(const char *path, uint32_t tex_free, uint32_t tex_used) {
    FILE *fp = fopen(path, "rb");
    long len;
    uint8_t *f;
    uint32_t streams, rva, i;
    Dump d = { 0 };
    PmcPoolStats s;
    double total;
    char label[600];

    snprintf(label, sizeof(label), "open %s", path);
    if (!fp) { check(0, label); return; }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f = malloc((size_t)len);
    if (!f || fread(f, 1, (size_t)len, fp) != (size_t)len) { check(0, "read the dump"); fclose(fp); return; }
    fclose(fp);

    streams = u32_at(f + 8);
    rva = u32_at(f + 12);
    for (i = 0; i < streams; i++) {
        const uint8_t *e = f + rva + i * 12;
        if (u32_at(e) == 9) {   /* Memory64ListStream */
            const uint8_t *m = f + u32_at(e + 8);
            uint64_t count = u64_at(m), at = u64_at(m + 8), k;
            d.r = malloc((size_t)count * sizeof(Range));
            for (k = 0; k < count; k++) {
                d.r[k].start = u64_at(m + 16 + k * 16);
                d.r[k].size = u64_at(m + 24 + k * 16);
                d.r[k].file = at;
                at += d.r[k].size;
            }
            d.n = (uint32_t)count;
        }
    }
    snprintf(label, sizeof(label), "%s has a Memory64List", path);
    check(d.n != 0, label);
    if (!d.n) { free(f); return; }
    qsort(d.r, d.n, sizeof(Range), by_start);
    d.f = f;
    d.len = (size_t)len;

    PmcPoolWalk(dump_read, &d, tex_free, tex_used, &s);
#define MIB(x) ((double)(x) / 1048576.0)
    printf("      texture free %u / %.2f MiB, in use %u / %.2f MiB\n", (unsigned)s.texture_free.nodes,
           MIB(s.texture_free.bytes), (unsigned)s.texture_in_use.nodes, MIB(s.texture_in_use.bytes));
    printf("      vb      free %u / %.2f MiB, in use %u / %.2f MiB\n", (unsigned)s.vb_free.nodes,
           MIB(s.vb_free.bytes), (unsigned)s.vb_in_use.nodes, MIB(s.vb_in_use.bytes));
    printf("      ib      free %u / %.2f MiB, in use %u / %.2f MiB\n", (unsigned)s.ib_free.nodes,
           MIB(s.ib_free.bytes), (unsigned)s.ib_in_use.nodes, MIB(s.ib_in_use.bytes));
    total = MIB(s.texture_free.bytes + s.texture_in_use.bytes + s.vb_free.bytes + s.vb_in_use.bytes +
                s.ib_free.bytes + s.ib_in_use.bytes);
    printf("      total %.2f MiB, unknown formats %u\n", total, (unsigned)s.unknown_formats);
    snprintf(label, sizeof(label), "%s walks with no unreadable node", path);
    check(s.texture_free.unreadable + s.texture_in_use.unreadable + s.vb_free.unreadable + s.vb_in_use.unreadable +
          s.ib_free.unreadable + s.ib_in_use.unreadable == 0, label);
    free(d.r);
    free(f);
}

int main(int argc, char **argv) {
    int i;
    synthetic();
    for (i = 1; i + 2 < argc; i += 3)
        real_dump(argv[i], (uint32_t)strtoul(argv[i + 1], NULL, 16), (uint32_t)strtoul(argv[i + 2], NULL, 16));
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
