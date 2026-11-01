/**
 * pool_stats.c — see pool_stats.h.
 */
#include "pool_stats.h"

#include <string.h>

#include "pmc_sites.gen.h"

/* A node: +0 key low (0x10000000 set while in use), +4 key high, +8 resource, +0xC owner, +0x10 next,
 * +0x14 previous. */
#define NODE_DWORDS   6
#define NODE_NEXT     4
#define KEY_IN_USE    0x10000000u

/* Texture lists are indexed [format bucket][log2 width]. */
#define TEXTURE_BUCKETS 200
#define TEXTURE_WIDTHS  16
#define BUFFER_BUCKETS  0x400

/* Buffer keys hold their size in units, in the low bits. */
#define IB_SIZE_MASK 0x1FFFu
#define IB_UNIT      1024u
#define VB_SIZE_MASK 0x7FFFu
#define VB_UNIT      2048u

/* A bound on one list, so a list being rewritten by another thread cannot loop the walk. */
#define MAX_NODES 200000u

#define FOURCC_DXT1 0x31545844u
#define FOURCC_DXT5 0x35545844u

uint32_t PmcTextureLevelBytes(uint32_t format, uint32_t width, uint32_t height) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (bw == 0) bw = 1;
    if (bh == 0) bh = 1;
    switch (format) {
    case FOURCC_DXT1: return bw * bh * 8;
    case FOURCC_DXT5: return bw * bh * 16;
    case 21: case 22: case 114: return width * height * 4;      /* A8R8G8B8, X8R8G8B8, R32F */
    case 23: case 24: case 25: case 26: case 51: return width * height * 2;   /* 565, 1555, 4444, A8L8 */
    case 28: case 50: return width * height;                    /* A8, L8 */
    default: return 0;
    }
}

/* A node's bytes from its 64-bit key: low dword ((levels | kind << 12) << 12) | (log2h << 4) | log2w,
 * where kind 2 is a cube; the high dword is the format. */
static uint64_t texture_bytes(const uint32_t node[NODE_DWORDS], PmcPoolStats *out) {
    uint32_t lo = node[0] & ~KEY_IN_USE, format = node[1];
    uint32_t w = 1u << (lo & 0xF), h = 1u << ((lo >> 4) & 0xFF);
    uint32_t levels = (lo >> 12) & 0xFFF, kind = (lo >> 24) & 0xF, i;
    uint64_t total = 0;

    if (PmcTextureLevelBytes(format, 1, 1) == 0) {
        out->unknown_formats++;
        for (i = 0; i < levels; i++) {
            uint32_t lw = w >> i, lh = h >> i;
            total += (uint64_t)(lw ? lw : 1) * (lh ? lh : 1) * 4;
        }
    } else {
        for (i = 0; i < levels; i++) {
            uint32_t lw = w >> i, lh = h >> i;
            total += PmcTextureLevelBytes(format, lw ? lw : 1, lh ? lh : 1);
        }
    }
    return kind == 2 ? total * 6 : total;
}

static void walk_one(PmcPoolRead read, void *ctx, uint32_t head, PmcPoolList *list, int texture,
                     uint32_t size_mask, uint32_t unit, PmcPoolStats *out) {
    uint32_t n = head, guard = 0;
    while (n && guard++ < MAX_NODES) {
        uint32_t node[NODE_DWORDS];
        if (!read(ctx, n, node, sizeof(node))) {
            list->unreadable++;
            return;
        }
        list->nodes++;
        list->bytes += texture ? texture_bytes(node, out) : (uint64_t)(node[0] & size_mask) * unit;
        n = node[NODE_NEXT];
    }
}

static void walk_textures(PmcPoolRead read, void *ctx, uint32_t global, PmcPoolList *list, PmcPoolStats *out) {
    uint32_t table, i, j;
    if (!read(ctx, global, &table, 4) || !table) return;
    for (i = 0; i < TEXTURE_BUCKETS; i++) {
        uint32_t widths;
        if (!read(ctx, table + i * 4, &widths, 4) || !widths) continue;
        for (j = 0; j < TEXTURE_WIDTHS; j++) {
            uint32_t head;
            if (read(ctx, widths + j * 4, &head, 4) && head)
                walk_one(read, ctx, head, list, 1, 0, 0, out);
        }
    }
}

static void walk_buffers(PmcPoolRead read, void *ctx, uint32_t global, PmcPoolList *list,
                         uint32_t size_mask, uint32_t unit, PmcPoolStats *out) {
    uint32_t table, i;
    if (!read(ctx, global, &table, 4) || !table) return;
    for (i = 0; i < BUFFER_BUCKETS; i++) {
        uint32_t head;
        if (read(ctx, table + i * 4, &head, 4) && head)
            walk_one(read, ctx, head, list, 0, size_mask, unit, out);
    }
}

void PmcPoolWalk(PmcPoolRead read, void *ctx, uint32_t texture_free, uint32_t texture_in_use, PmcPoolStats *out) {
    memset(out, 0, sizeof(*out));
    walk_textures(read, ctx, texture_free, &out->texture_free, out);
    walk_textures(read, ctx, texture_in_use, &out->texture_in_use, out);
    walk_buffers(read, ctx, texture_in_use + PMC_POOL_IB_IN_USE_OFF, &out->ib_in_use, IB_SIZE_MASK, IB_UNIT, out);
    walk_buffers(read, ctx, texture_in_use + PMC_POOL_VB_IN_USE_OFF, &out->vb_in_use, VB_SIZE_MASK, VB_UNIT, out);
    walk_buffers(read, ctx, texture_in_use + PMC_POOL_IB_FREE_OFF, &out->ib_free, IB_SIZE_MASK, IB_UNIT, out);
    walk_buffers(read, ctx, texture_in_use + PMC_POOL_VB_FREE_OFF, &out->vb_free, VB_SIZE_MASK, VB_UNIT, out);
}
