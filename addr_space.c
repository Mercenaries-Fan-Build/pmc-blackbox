/**
 * addr_space.c — see addr_space.h.
 *
 * VirtualQuery takes no loader lock and allocates nothing, so the sample is safe from the crash
 * handler, and it still works when the process has no address space left.
 */
#include "addr_space.h"

/* The Windows values, spelled here so the tally builds and runs on the host. */
#define VM_MEM_COMMIT  0x00001000u
#define VM_MEM_RESERVE 0x00002000u
#define VM_MEM_FREE    0x00010000u
#define VM_MEM_PRIVATE 0x00020000u
#define VM_MEM_MAPPED  0x00040000u
#define VM_MEM_IMAGE   0x01000000u
#define VM_LARGE_ADDRESS_AWARE 0x0020u

void PmcVmTallyRegion(PmcVmTally *t, uint32_t state, uint32_t type, uint64_t size) {
    t->regions++;
    if (state == VM_MEM_FREE) {
        t->free_bytes += size;
        if (size > t->largest_free) t->largest_free = size;
    } else if (state == VM_MEM_RESERVE) {
        t->reserved_bytes += size;
    } else if (state == VM_MEM_COMMIT) {
        if (type == VM_MEM_PRIVATE) t->private_bytes += size;
        else if (type == VM_MEM_IMAGE) t->image_bytes += size;
        else if (type == VM_MEM_MAPPED) t->mapped_bytes += size;
    }
}

int PmcIsLargeAddressAware(uint16_t characteristics) {
    return (characteristics & VM_LARGE_ADDRESS_AWARE) != 0;
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

extern void pmc_log(const char *source, const char *fmt, ...);

static unsigned long kib(uint64_t bytes) {
    return (unsigned long)(bytes / 1024);
}

void PmcVmLogSample(const char *source, const char *what) {
    SYSTEM_INFO si;
    PmcVmTally t;
    uintptr_t at, top;

    GetSystemInfo(&si);
    at = (uintptr_t)si.lpMinimumApplicationAddress;
    top = (uintptr_t)si.lpMaximumApplicationAddress;
    memset(&t, 0, sizeof(t));

    while (at < top) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;
        if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            pmc_log(source, "%s: VirtualQuery failed at %08lX, error %lu", what, (unsigned long)at, GetLastError());
            return;
        }
        PmcVmTallyRegion(&t, mbi.State, mbi.Type, mbi.RegionSize);
        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }

    pmc_log(source, "%s free_kib=%lu largest_kib=%lu private_kib=%lu image_kib=%lu mapped_kib=%lu "
            "reserved_kib=%lu regions=%lu",
            what, kib(t.free_bytes), kib(t.largest_free), kib(t.private_bytes), kib(t.image_bytes),
            kib(t.mapped_bytes), kib(t.reserved_bytes), (unsigned long)t.regions);
}

static DWORD WINAPI SamplerThread(LPVOID param) {
    (void)param;
    for (;;) {
        PmcVmLogSample("vmem", "sample");
        Sleep(PMC_VM_PERIOD_MS);
    }
    return 0;
}

void PmcVmStartSampler(void) {
    SYSTEM_INFO si;
    const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);

    GetSystemInfo(&si);
    pmc_log("blackbox", "ADDRSPACE laa=%d limit=%08lX",
            PmcIsLargeAddressAware(nt->FileHeader.Characteristics),
            (unsigned long)(uintptr_t)si.lpMaximumApplicationAddress);
    CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
}

#endif /* _WIN32 */
