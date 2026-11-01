#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

/* Install the VEH and the detour on UnhandledExceptionFilter that log the
 * faulting site (EIP, code, AV target, registers, stack) to pmc_blackbox.log
 * [crash]. `minidump` also arms a full-memory dump for a fatal crash. */
void InstallCrashHandler(int minidump);

#endif /* CRASH_HANDLER_H */
