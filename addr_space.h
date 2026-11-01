/**
 * addr_space.h — the process's address space, measured.
 *
 * The game is 32-bit, so a long session can run out of address space while the machine has memory
 * to spare. A CreateTexture that returns E_OUTOFMEMORY says nothing about which limit it hit; these
 * samples do: `free_kib` near zero, or a `largest_kib` too small for the request, against a `limit`
 * that shows whether the exe is large-address-aware.
 *
 * Sampled every PMC_VM_PERIOD_MS from a background thread as lines of source `[vmem]`, and once more
 * inside the crash record.
 */
#ifndef PMC_ADDR_SPACE_H
#define PMC_ADDR_SPACE_H

#include <stdint.h>

#define PMC_VM_PERIOD_MS 30000

typedef struct {
    uint64_t free_bytes;
    uint64_t largest_free;
    uint64_t private_bytes;   /* committed MEM_PRIVATE */
    uint64_t image_bytes;     /* committed MEM_IMAGE */
    uint64_t mapped_bytes;    /* committed MEM_MAPPED */
    uint64_t reserved_bytes;
    uint32_t regions;
} PmcVmTally;

/* Add one VirtualQuery region; `state` and `type` are the Windows MEM_* values. Host-tested. */
void PmcVmTallyRegion(PmcVmTally *t, uint32_t state, uint32_t type, uint64_t size);

/* Whether a PE image's Characteristics field has IMAGE_FILE_LARGE_ADDRESS_AWARE. Host-tested. */
int PmcIsLargeAddressAware(uint16_t characteristics);

/* Log one sample of the whole address space as a line of `source`, starting with `what`. The crash
 * record passes its own source, because loadprobe reads a crash block as one unbroken run of lines. */
void PmcVmLogSample(const char *source, const char *what);

/* Log the `ADDRSPACE` identity line and start the periodic sampler thread. */
void PmcVmStartSampler(void);

#endif /* PMC_ADDR_SPACE_H */
