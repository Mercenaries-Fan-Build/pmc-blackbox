/**
 * pool_stats.h — what the game's D3D resource pool holds.
 *
 * The pool caches textures, vertex buffers and index buffers by exact shape and never frees a texture,
 * so its free lists show how much memory a session has stranded. Layouts and evidence:
 * docs/reverse_engineer/d3d_resource_pool.md in the research repository.
 *
 * Portable: memory comes through a read callback, so the walk runs over the live process in the crash
 * handler and over a captured minidump in test_pool_stats.c.
 */
#ifndef PMC_POOL_STATS_H
#define PMC_POOL_STATS_H

#include <stdint.h>

/* Copy `n` bytes at `va` into `out`; 1 on success, 0 when they are not readable. */
typedef int (*PmcPoolRead)(void *ctx, uint32_t va, void *out, uint32_t n);

typedef struct {
    uint32_t nodes;
    uint32_t unreadable;   /* lists cut short by a node that could not be read */
    uint64_t bytes;
} PmcPoolList;

typedef struct {
    PmcPoolList texture_free, texture_in_use;
    PmcPoolList vb_free, vb_in_use;
    PmcPoolList ib_free, ib_in_use;
    uint32_t    unknown_formats;   /* texture nodes counted at 4 bytes a pixel */
} PmcPoolStats;

/* Walk every list. The two addresses are the sites `texture_free` and `texture_in_use`. */
void PmcPoolWalk(PmcPoolRead read, void *ctx, uint32_t texture_free, uint32_t texture_in_use, PmcPoolStats *out);

/* The bytes of one texture level, 0 for a format the table does not hold. */
uint32_t PmcTextureLevelBytes(uint32_t format, uint32_t width, uint32_t height);

#endif /* PMC_POOL_STATS_H */
