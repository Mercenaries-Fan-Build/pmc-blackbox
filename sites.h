/**
 * sites.h — the game addresses this build of the exe keeps, found in the running image.
 *
 * pmc_sites.gen.h is generated from mercs2_formats::pmc_sites, the table m2-sdk's own site resolvers
 * sit beside, so a site has one definition. This file is the scanner for it: a byte pattern with `??`
 * wildcards must match exactly once in .text or nothing is resolved and the reason is logged. A build
 * the table does not fit therefore gets no writes and no post-mortems, rather than wrong ones.
 */
#ifndef PMC_SITES_H
#define PMC_SITES_H

#include <stddef.h>
#include <stdint.h>

#include "pmc_sites.gen.h"

/**
 * Resolve every site over `text`, the bytes of .text mapped at `text_va`. Returns 1 and fills `values`
 * when every site matches exactly once; returns 0 and leaves the reason in `why` otherwise, naming each
 * site's match count. Portable: tested on the host by test_sites.c, on real executables by
 * `make check-sites`.
 */
int PmcSitesResolve(const uint8_t *text, uint32_t text_va, uint32_t text_size,
                    uint32_t values[PMC_SITE_COUNT], char *why, size_t why_len);

/* Resolve from the running image, now if .text is readable and otherwise on a thread, because SecuROM
 * decrypts .text after the image is mapped. Logs each site, or why they did not resolve. */
void PmcSitesStart(void);

/* A site's value once resolved, 0 before that and on a build they do not fit. */
uint32_t PmcSite(int site);

/* Whether `eip` lies in a function site. */
int PmcSiteHolds(int site, uint32_t eip);

#endif /* PMC_SITES_H */
