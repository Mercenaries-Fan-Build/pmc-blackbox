/**
 * lua_log_hook.h — native capture of the game's stripped-out logging stream
 *
 * The production build redirected ~all of its debug/log functions (Lua `print`,
 * `Debug.Printf`, and ~700 subsystem loggers) to a single shared no-op stub,
 * whose address differs per build and is found in the running image (log_stub.h).
 * We MinHook that stub's CODE so every funneled message is routed
 * to pmc_log() and lands in pmc_blackbox.log natively, with no separate ASI.
 *
 * Hooking the .text stub (not the .rdata func-pointer slots) is what makes this
 * safe on the SecuROM-protected EXE: register tables stay pristine (anti-tamper
 * not tripped) and .text MinHook detours are tolerated, as compat_hooks proves.
 * This makes the logging that used to live in dlc_enable.asi a built-in feature
 * of pmc_bb, and an essential debugging layer for everything that funnels there.
 */
#pragma once

/**
 * Install the MinHook detour on the shared stripped-log stub, found in the
 * running image by FindLogStub. When it cannot be found, nothing is hooked and
 * the reason is logged.
 * Call once from DllMain AFTER InstallCompatHooks() (MinHook init + pmc_log)
 * and BEFORE LoadASIPlugins(). No .rdata writes.
 *
 * The hook detours the game's hot shared log stub and runs per-call stack
 * resolution + formatting on every funneled call, so it is installed ONLY for a
 * diagnostic run (the caller gates this behind PMC_VERBOSE_LOG). Every captured
 * line is emitted with its @script:line location; world-load milestones are also
 * echoed under the "world" source.
 *
 * Returns 1 on success, 0 on failure (logged).
 */
int InstallLuaLogHook(void);
