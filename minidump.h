/**
 * minidump.h — a full-memory minidump when the game dies.
 *
 * Off unless the run asks for it with PMC_CRASH_MINIDUMP: a dump of this game is about 1.5 GB, written
 * to the game folder as pmc_crash_<date>_<time>.dmp.
 *
 * The crash this exists for can be running out of address space, so nothing waits for the crash:
 * dbghelp is loaded, the writer thread started and a block of address space reserved while there is
 * still room, and the block is released first when the dump starts.
 */
#ifndef PMC_MINIDUMP_H
#define PMC_MINIDUMP_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Get ready to write a dump, on a thread once the loader lock is free. Logs why when it cannot: a run
 * that asked for a dump and cannot have one says so at startup rather than after the crash. */
void PmcMinidumpArm(void);

/* Write the dump for the fault in `ep`, once per process, and wait for it. Does nothing unless armed. */
void PmcMinidumpWrite(EXCEPTION_POINTERS *ep);

#endif /* PMC_MINIDUMP_H */
