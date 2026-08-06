/* test_sha256.c — host test for the digest that backs the `BUILD` records.
 *
 * Run with `make check`. Compiles natively (clang/gcc), NOT for Windows: the
 * DLL is a 32-bit Windows binary that cannot be executed on the machines this
 * is developed on, so without this the digest would ship unverified. A wrong
 * SHA-256 fails silently — the log carries 64 plausible hex characters that
 * match nothing on the other side.
 *
 * Covers:
 *   1. FIPS 180-4 / RFC 6234 vectors.
 *   2. Chunked feeding == one-shot. `digest_range` streams a file through a
 *      1 MiB buffer in arbitrary-sized ReadFile chunks, so a buffering bug
 *      there would change the digest of every file over 1 MiB and nothing
 *      smaller would catch it.
 *   3. The qsha256 construction: head || tail || le64(size), assembled exactly
 *      as build_id.c assembles it, checked against an independently computed
 *      digest of the same concatenation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

static int fails;

static void hex_of(const void *p, size_t n, char *out)
{
    pmc_sha256 c;
    pmc_sha256_init(&c);
    pmc_sha256_update(&c, p, n);
    pmc_sha256_final_hex(&c, out);
}

static void check(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n       got  %s\n       want %s\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    char got[65], want[65];

    puts("known-answer vectors");
    hex_of("", 0, got);
    check("empty", got,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    hex_of("abc", 3, got);
    check("abc", got,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, got);
    check("448-bit", got,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    {   /* one million 'a' — the length-padding path with a large 64-bit count */
        pmc_sha256 c;
        char blk[1000];
        int i;
        memset(blk, 'a', sizeof(blk));
        pmc_sha256_init(&c);
        for (i = 0; i < 1000; i++) pmc_sha256_update(&c, blk, sizeof(blk));
        pmc_sha256_final_hex(&c, got);
        check("1e6 x 'a'", got,
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    puts("\nchunked feeding matches one-shot");
    {
        size_t n = 3 * 1024 * 1024 + 12345;   /* > 1 MiB, not block-aligned */
        unsigned char *big = malloc(n);
        size_t i, off;
        unsigned int x = 0x12345678u;
        pmc_sha256 c;
        /* deterministic filler */
        for (i = 0; i < n; i++) { x = x * 1103515245u + 12345u; big[i] = (unsigned char)(x >> 16); }

        hex_of(big, n, want);

        /* ragged chunk sizes, including ones that straddle the 64-byte block */
        pmc_sha256_init(&c);
        for (off = 0; off < n; ) {
            size_t step = 1 + ((off * 7919) % 100003);
            if (off + step > n) step = n - off;
            pmc_sha256_update(&c, big + off, step);
            off += step;
        }
        pmc_sha256_final_hex(&c, got);
        check("ragged chunks", got, want);
        free(big);
    }

    puts("\nqsha256 construction: head || tail || le64(size)");
    {
        /* A stand-in for a >1 GiB WAD. The chunk size is scaled down so the
         * test stays fast; the SHAPE under test is the concatenation order and
         * the little-endian length, which is what the two sides must agree on. */
        const size_t CHUNK = 4096;
        const unsigned long long SIZE = 2565537792ull;   /* a real vz.wad size */
        unsigned char head[4096], tail[4096], le[8];
        unsigned char *joined = malloc(CHUNK * 2 + 8);
        pmc_sha256 c;
        size_t i;

        for (i = 0; i < CHUNK; i++) { head[i] = (unsigned char)(i * 3); tail[i] = (unsigned char)(i * 5 + 1); }
        for (i = 0; i < 8; i++) le[i] = (unsigned char)((SIZE >> (8 * i)) & 0xFF);

        /* Independent reference: one flat buffer, one update. */
        memcpy(joined, head, CHUNK);
        memcpy(joined + CHUNK, tail, CHUNK);
        memcpy(joined + CHUNK * 2, le, 8);
        hex_of(joined, CHUNK * 2 + 8, want);

        /* Same sequence build_id.c performs: two ranged updates then the size. */
        pmc_sha256_init(&c);
        pmc_sha256_update(&c, head, CHUNK);
        pmc_sha256_update(&c, tail, CHUNK);
        pmc_sha256_update(&c, le, 8);
        pmc_sha256_final_hex(&c, got);
        check("three updates == flat buffer", got, want);

        /* The little-endian encoding itself, spelled out so a big-endian
         * mistake on either side of the cross-repo agreement is visible. */
        {
            char le_hex[17];
            for (i = 0; i < 8; i++) sprintf(le_hex + i * 2, "%02x", le[i]);
            /* 2565537792 == 0x98EB0000, so LE is 00 00 eb 98 then four zeros. */
            check("le64(2565537792)", le_hex, "0000eb98" "00000000");
        }
        free(joined);
    }

    printf("\n%s\n", fails ? "FAILED" : "all digest tests passed");
    return fails ? 1 : 0;
}
