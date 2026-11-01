/* pmc_sites.gen.h -- GENERATED from mercs2_formats::pmc_sites by
 * `mercs2_probe --bin pmc_sites_header`. Do not edit by hand. */
#ifndef PMC_SITES_GEN_H
#define PMC_SITES_GEN_H

#define PMC_SITE_COUNT 8

#define PMC_SITE_ENTITY_VTABLE 0
#define PMC_SITE_PRMG_KEYS 1
#define PMC_SITE_PRMG_HANDLES 2
#define PMC_SITE_SPAWN_WALK 3
#define PMC_SITE_PRMG_DRAW_A 4
#define PMC_SITE_PRMG_DRAW_B 5
#define PMC_SITE_TEXTURE_FREE 6
#define PMC_SITE_TEXTURE_IN_USE 7

#define PMC_POOL_IB_IN_USE_OFF 0x4
#define PMC_POOL_VB_IN_USE_OFF 0x8
#define PMC_POOL_IB_FREE_OFF 0xc
#define PMC_POOL_VB_FREE_OFF 0x10

/* operand_at >= 0: the site is the 32-bit value that far into the match.
 * operand_at < 0: the site is the match itself, a function of function_len bytes. */
typedef struct {
    const char *name;
    const char *pattern;
    int operand_at;
    unsigned function_len;
} PmcSiteDef;

static const PmcSiteDef PMC_SITE_DEFS[PMC_SITE_COUNT] = {
    { "entity_vtable", "56 57 8b f9 8b 87 84 01 00 00 85 c0 c7 07 ?? ?? ?? ?? c7 47 10 ?? ?? ?? ?? c7 47 64 ?? ?? ?? ?? 74 38 8b 08 8b 50 04 89 51 04", 14, 0x0 },
    { "prmg_keys", "85 a0 01 00 00 f3 0f 10 44 24 60 68 00 01 00 00 be ?? ?? ?? ?? f3 0f 11 85 a4 01 00 00 e8 ?? ?? ?? ?? 83 c4 0c 85 c0 7d 07", 17, 0x0 },
    { "prmg_handles", "3a 00 83 c4 0c 85 c0 7d 07 b8 ?? ?? ?? ?? eb 07 8d 04 85 ?? ?? ?? ?? 8b 00 8b 4c 24 30 68 00 01 00 00 be ?? ?? ?? ?? 89 45 00", 19, 0x0 },
    { "spawn_walk", "83 ec 34 83 3d ?? ?? ?? ?? 00 53 55 56 8b f1 57 89 74 24 1c 75 0f ba ?? ?? ?? ?? e8 ?? ?? ?? ?? a3 ?? ?? ?? ?? a1 ?? ?? ?? ?? 0f b7 88 92 2b 00 00", -1, 0x2aa },
    { "prmg_draw_a", "83 ec 0c 55 8b 6c 24 14 33 c0 39 45 04 89 44 24 08 0f 8e ?? ?? ?? ?? 53 56 89 44 24 0c 57 8b ff 8b 7d 08 03 7c 24 10 8b 45 14 85 c0 8b 1f 89 44 24 18 74 64 8b 75 10 85 f6 74 5d 83 bb fc 00 00 00 00", -1, 0x198 },
    { "prmg_draw_b", "83 ec 0c 57 8b 7c 24 14 33 c0 39 47 04 89 44 24 0c 0f 8e ?? ?? ?? ?? 53 55 89 44 24 0c 56 8b ff 8b 6f 08 03 6c 24 10 8b 47 14 85 c0 8b 5d 04 89 44 24 14 74 64 8b 77 10 85 f6 74 5d 83 bb fc 00 00 00 00", -1, 0x1b4 },
    { "texture_free", "f3 0f 2a 44 24 0c f3 0f 11 44 24 0c 8b 4c 24 0c c1 e9 17 81 e1 ff 00 00 00 83 e9 7f 3d c8 00 00 00 0f b7 d1 73 05 0f b7 c0 eb 03 c1 e8 19 0f bf c8 a1 ?? ?? ?? ?? 53 0f bf d2 03 c9 55 03 c9 8b 04 01 03 d2 56 03 d2 57", 50, 0x0 },
    { "texture_in_use", "70 10 89 37 8b 70 04 81 08 00 00 00 10 89 70 04 8b 35 ?? ?? ?? ?? 89 58 10 89 58 14 8b 0c 31 03 ca 8b 11 3b d3 74 06 89 50 10", 18, 0x0 },
};

#endif /* PMC_SITES_GEN_H */
