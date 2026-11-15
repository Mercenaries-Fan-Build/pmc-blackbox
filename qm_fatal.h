/**
 * qm_fatal.h — the Quartermaster's no-dialog failure: a native message box, then exit.
 *
 * Quartermaster (qm) splices an error policy onto retail Lua. When a mod's hook handler or library
 * fails and retail's own dialog cannot be shown (the loading screen, the front end before its GUI,
 * no local player), qm prints one line through the game's Lua `print`:
 *
 *     [qm] FATAL-NODIALOG <owner> at <hook>: <error>
 *
 * Every log variant of pmc_bb watches the print stub for that line (lua_log_hook.c), whether or not
 * PMC_VERBOSE_LOG is set. On a match it writes the line to pmc_blackbox.log, shows the text in a
 * native message box and ends the process when the box is dismissed: the game never continues past
 * a failure. qm requests the `Exiting` game state right after the print, for the setups where
 * nothing watches (the quiet variants); here that request never runs.
 */
#pragma once

/** The prefix qm's no-dialog failure line starts with, the trailing space included. */
#define QM_FATAL_PREFIX "[qm] FATAL-NODIALOG "

/** The process exit code after a no-dialog failure. */
#define QM_FATAL_EXIT_CODE 3

/**
 * The failure text of `line` (what follows QM_FATAL_PREFIX) when `line` is qm's no-dialog failure
 * line, or NULL when it is not. A line that is only the prefix is not one. Portable C: tested on the
 * host by test_qm_fatal.c.
 */
const char *QmFatalText(const char *line);

#ifdef _WIN32
/**
 * Stop the game for qm's no-dialog failure line `line` (QmFatalText(line) is non-NULL): log it and
 * flush, minimize the game's windows, show the failure in a message box on a thread of its own while
 * the calling (game) thread waits, then terminate the process with QM_FATAL_EXIT_CODE. Runs once; a
 * second call while the first is showing its box waits with it. Never returns.
 */
__declspec(noreturn) void QmFatalStop(const char *line);
#endif
