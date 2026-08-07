/* sha256.h — plain SHA-256 (FIPS 180-4).
 *
 * Deliberately free of <windows.h> so it can be compiled and tested on the
 * host. The build target is a 32-bit Windows DLL that cannot be run in the
 * environments this is developed in, and a wrong digest here would fail
 * silently — the log would carry 64 plausible hex characters that match
 * nothing. So the digest lives in portable C with a host test beside it
 * (`make check`), and only the file/module plumbing stays Windows-only.
 */
#ifndef PMC_SHA256_H
#define PMC_SHA256_H

#include <stddef.h>

typedef struct {
    unsigned int h[8];
    unsigned long long len;      /* total bytes fed */
    unsigned char buf[64];
    unsigned int n;              /* bytes currently buffered */
} pmc_sha256;

void pmc_sha256_init(pmc_sha256 *c);
void pmc_sha256_update(pmc_sha256 *c, const void *data, size_t len);
/* Writes 64 lowercase hex chars plus a NUL. `hex` must hold 65 bytes. */
void pmc_sha256_final_hex(pmc_sha256 *c, char *hex);

#endif /* PMC_SHA256_H */
