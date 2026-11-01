/**
 * test_addr_space.c — host test of PmcVmTallyRegion and PmcIsLargeAddressAware.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "addr_space.h"

#include <stdio.h>
#include <string.h>

/* The Windows MEM_* values. */
#define COMMIT  0x00001000u
#define RESERVE 0x00002000u
#define FREE    0x00010000u
#define PRIVATE 0x00020000u
#define MAPPED  0x00040000u
#define IMAGE   0x01000000u

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

int main(void) {
    PmcVmTally t;
    memset(&t, 0, sizeof(t));

    /* The state of a free region's Type field is undefined by the API: it is ignored. */
    PmcVmTallyRegion(&t, FREE, 0xFFFFFFFFu, 0x100000);
    PmcVmTallyRegion(&t, FREE, 0, 0x300000);
    PmcVmTallyRegion(&t, FREE, 0, 0x200000);
    PmcVmTallyRegion(&t, COMMIT, PRIVATE, 0x26226000);
    PmcVmTallyRegion(&t, COMMIT, PRIVATE, 0x1000);
    PmcVmTallyRegion(&t, COMMIT, IMAGE, 0x2000);
    PmcVmTallyRegion(&t, COMMIT, MAPPED, 0x3000);
    PmcVmTallyRegion(&t, RESERVE, PRIVATE, 0x4000);

    check(t.free_bytes == 0x600000, "free regions add up");
    check(t.largest_free == 0x300000, "the largest hole is the largest single free region");
    check(t.private_bytes == 0x26227000, "committed private adds up");
    check(t.image_bytes == 0x2000 && t.mapped_bytes == 0x3000, "image and mapped are separate");
    check(t.reserved_bytes == 0x4000, "a reservation is not committed");
    check(t.regions == 8, "every region is counted");

    check(PmcIsLargeAddressAware(0x0123) == 1, "Characteristics 0x0123, the LAA exes, is large-address-aware");
    check(PmcIsLargeAddressAware(0x0103) == 0, "Characteristics 0x0103, retail, is not");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
