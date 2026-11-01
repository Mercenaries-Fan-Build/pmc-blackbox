/**
 * test_crash_win.c — a Windows exe that does to pmc_bb what the game does, then dies.
 *
 * The crash paths are Windows code a host test cannot reach, so this runs them: it loads a pmc_bb DLL
 * given by path, replaces the top-level filter the way the game's CRT startup does (the old filter is
 * dropped), and takes three access violations: one a vectored handler survives, one the top-level
 * filter survives, and one nothing survives. Run it from a folder that holds the DLL, with
 * PMC_CRASH_MINIDUMP=1 if a dump is wanted, then read pmc_blackbox.log:
 *
 *   ==== VEH EXCEPTION       three times, one per fault
 *   ==== UNHANDLED EXCEPTION once, for the fatal fault only. Two would mean a fault the filter
 *                            recovered from was recorded as fatal.
 *   minidump: wrote ...      with PMC_CRASH_MINIDUMP=1, and exactly one pmc_crash_*.dmp beside the log
 *
 * This is not the game, so the sites do not resolve: the record says so, and that path is under test too.
 *
 *   make pmc_crash_test.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* Each fault reads through ECX into EAX: `mov eax,[ecx]`, 8B 01, two bytes. */
#define FAULT_BYTES 2
#define FAULT() __asm__ volatile ("xorl %%ecx, %%ecx\n\tmovl (%%ecx), %%eax" : : : "eax", "ecx")

static volatile LONG g_filter_recovers;

static LONG WINAPI GameLikeFilter(EXCEPTION_POINTERS *ep) {
    if (!InterlockedExchange(&g_filter_recovers, 0)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Eip += FAULT_BYTES;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI SkipFault(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Eip += FAULT_BYTES;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(int argc, char **argv) {
    PVOID skip;
    HMODULE dll;

    if (argc != 2) {
        fprintf(stderr, "usage: pmc_crash_test <path to a pmc_bb DLL with the log-stack>\n");
        return 2;
    }
    dll = LoadLibraryA(argv[1]);
    if (!dll) {
        fprintf(stderr, "could not load %s: error %lu\n", argv[1], GetLastError());
        return 2;
    }
    SetUnhandledExceptionFilter(GameLikeFilter);

    /* Give the sampler and the minidump writer time to start. */
    Sleep(1000);

    skip = AddVectoredExceptionHandler(0, SkipFault);   /* after pmc_bb's, which is first */
    FAULT();
    RemoveVectoredExceptionHandler(skip);
    printf("survived the fault a vectored handler took\n");
    fflush(stdout);

    g_filter_recovers = 1;
    FAULT();
    printf("survived the fault the top-level filter took; the next one is fatal\n");
    fflush(stdout);

    FAULT();
    return 0;
}
