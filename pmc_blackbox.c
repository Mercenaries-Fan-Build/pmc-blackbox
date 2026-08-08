/**
 * pmc_bb — SecuROM Spoof + Debug Console + ASI Loader + Compat Hooks
 *                for Mercenaries 2: World in Flames
 *
 * Self-contained entry point that replaces the need for a separate ASI loader
 * (xinput1_3.dll / dinput8.dll proxy). Loaded via the game's import table.
 *
 * IMPORTANT: The game's import table must reference this DLL by whatever
 * filename it is installed under — there is no required name, and nothing here
 * depends on one. The exe patcher (mercs2-crack-game) handles this: it injects
 * the installed filename into the import table and binds BlackboxEntry by
 * ordinal #1. The no-crack variants are not imported at all; dxwrapper loads
 * them by path via LoadCustomDllPath.
 *
 * Responsibilities:
 *   1. Creates the SecuROM v7 spoof Event (mandatory for game boot)
 *   2. Allocates a debug console window with stdout/stderr redirection
 *   3. Installs crash handler + Lua logging hooks
 *   4. Fixes underground spawn validation
 *   5. Discovers and LoadLibrary's all .asi plugins from:
 *      - Game root directory
 *      - scripts/ subfolder
 *      - plugins/ subfolder
 *      - update/ subfolder
 *      Always all four; it never checks for other loaders (see "Coexisting
 *      with other ASI loaders" below).
 *   6. Reports load success/failure for each plugin
 *   7. Exports pmc_log() — centralized logging API for all ASI plugins.
 *      Writes timestamped, source-tagged lines to the console AND to a
 *      single pmc_blackbox.log file on disk.
 *
 * The DLL exports BlackboxEntry by ordinal #1 (the game's import table
 * resolves this by ordinal) and pmc_log by name.
 *
 * Not every build contains all of the above: the three features (SecuROM
 * spoof, ASI loader, log-stack) are independently compiled out to produce the
 * six published variants. See "Build variants" below and the matrix at the top
 * of the Makefile.
 *
 * Build (MinGW cross-compile):
 *   make all                       (all six variants)
 *   make pmc_bb_fully_loaded.dll   (just one)
 *
 * Architecture: 32-bit (x86) Windows DLL — Mercenaries 2 is a 32-bit game.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "lua_log_hook.h"
#include "crash_handler.h"
#include "build_id.h"

/* Version string embedded in the startup banner. The build injects the exact
 * git tag via -DPMC_BLACKBOX_VERSION (see Makefile / release.yml) so the DLL
 * always reports the version it was released as. The fallback below is only used
 * for an ad-hoc build outside the Makefile (no -D, no git), never for a release. */
#ifndef PMC_BLACKBOX_VERSION
#define PMC_BLACKBOX_VERSION "0.0.0-dev"
#endif
/* --- Build variants ---
 *
 * Three independent features, each compiled out by its own flag:
 *
 *   crack   SecuROM v7 event spoof            -DPMC_DISABLE_SECUROM_EVENT
 *   asi     ASI loader, four search paths -DPMC_DISABLE_ASI_LOADER
 *   log     the log-stack                     -DPMC_DISABLE_LOG_STACK
 *
 * The log-stack is the console, pmc_blackbox.log, the pmc_log transport, the
 * crash handler, the Lua hooks, and the BUILD/LOADER run-identity records. It
 * is one flag rather than six because those parts are not independently
 * useful: the crash handler and the run-identity records have nowhere to write
 * without the transport, and a transport with no producers is an open file
 * handle that never gets a line. The Makefile pairs this flag with dropping
 * the sources outright (crash_handler.c, lua_log_hook.c, build_id.c, sha256.c
 * and all of MinHook), so a quiet build does not merely skip that code, it
 * does not contain it.
 *
 * pmc_log and pmc_log_flush stay EXPORTED in every variant, as inert stubs
 * where the log-stack is out, so a plugin that has found the module always
 * finds the symbol. A varying export set would make GetProcAddress("pmc_log")
 * succeed or fail based on which build the user installed — a difference the
 * plugin cannot anticipate and cannot do anything useful about. Plugins
 * brought in by some other loader call it even in builds whose own loader is
 * compiled out, so the symbol has to survive there too. */
#ifdef PMC_DISABLE_LOG_STACK
#  ifndef PMC_DISABLE_CRASH_HANDLER
#    define PMC_DISABLE_CRASH_HANDLER
#  endif
#  ifndef PMC_DISABLE_LUA_LOG_HOOK
#    define PMC_DISABLE_LUA_LOG_HOOK
#  endif
#endif

#define SECUROM_XOR_KEY 0x19EA3FD3

/* --- SecuROM event spoof ---
 *
 * Compiled out by -DPMC_DISABLE_SECUROM_EVENT, which is what separates the two
 * no-crack variants (asi_log, log_only) from the four cracked ones. Those
 * ship next to a LICENSED copy, loaded via dxwrapper instead of a patched import
 * table: the stock exe is satisfied by the machine's real SecuROM activation, so
 * this DLL must never fabricate the auth event. The spoof code is absent from
 * those binaries, not merely skipped at runtime. Which other features come along
 * is a separate question answered by the variant — see "Build variants" above. */
#ifndef PMC_DISABLE_SECUROM_EVENT
static HANDLE g_securomEvent = NULL;

static void CreateSecuROMEvent(void) {
    DWORD pid = GetCurrentProcessId();
    DWORD derived = pid ^ SECUROM_XOR_KEY;
    char event_name[32];
    wsprintfA(event_name, "v7_%04d", derived);
    g_securomEvent = CreateEventA(NULL, TRUE, TRUE, event_name);
}
#endif

/* --- Centralized logging ---
 *
 * The whole block through InitDebugConsole() is compiled out by
 * -DPMC_DISABLE_LOG_STACK, which substitutes the stubs at the far end. */
#ifndef PMC_DISABLE_LOG_STACK

static FILE*           g_logfile = NULL;
static CRITICAL_SECTION g_logLock;

static volatile LONG g_logPending = 0;
volatile LONG g_logDropped = 0;
/* Flush every line: a buffered tail is lost on a hard crash, and the missing
 * lines previously made an end-of-load fault (STATE_WAITFORSTREAMING) look like
 * an early-init one. fflush per line is cheap for a debug logger and keeps the
 * crash-time tail truthful. Raise this only if log volume becomes a hot path. */
#define LOG_FLUSH_THRESHOLD 1

static void InitLogFile(void) {
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep) *(sep + 1) = '\0';

    char log_path[MAX_PATH];
    wsprintfA(log_path, "%spmc_blackbox.log", exe_dir);
    g_logfile = fopen(log_path, "w");
    if (g_logfile)
        setvbuf(g_logfile, NULL, _IOFBF, 8192);

    InitializeCriticalSection(&g_logLock);
}

/**
 * Shared logging function exported for all ASI plugins.
 *
 * Formats a timestamped, source-tagged message and writes it to both the
 * debug console (stdout) and the pmc_blackbox.log file on disk.
 *
 * ASI plugins resolve this at runtime via GetProcAddress("pmc_log").
 */
__declspec(dllexport) void pmc_log(const char *source, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    wsprintfA(line, "[%02d:%02d:%02d.%03d] [%s] %s\n",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
              source ? source : "???", msg);

    /*
     * Use TryEnterCriticalSection so callers on latency-sensitive threads
     * (D3D9 rendering, audio) never block.  Dropped messages are counted
     * via g_logDropped and reported at shutdown.
     */
    if (!TryEnterCriticalSection(&g_logLock)) {
        InterlockedIncrement(&g_logDropped);
        return;
    }

    fputs(line, stdout);

    if (g_logfile)
        fputs(line, g_logfile);

    if (InterlockedIncrement(&g_logPending) >= LOG_FLUSH_THRESHOLD) {
        fflush(stdout);
        if (g_logfile) fflush(g_logfile);
        InterlockedExchange(&g_logPending, 0);
    }

    LeaveCriticalSection(&g_logLock);
}

__declspec(dllexport) void pmc_log_flush(void) {
    EnterCriticalSection(&g_logLock);
    fflush(stdout);
    if (g_logfile) fflush(g_logfile);
    InterlockedExchange(&g_logPending, 0);
    LeaveCriticalSection(&g_logLock);
}

/* --- Debug console --- */

static void InitDebugConsole(void) {
    AllocConsole();
    SetConsoleTitleA("Mercenaries 2 - PMC Blackbox");

    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    InitLogFile();

    pmc_log("blackbox", "============================================");
    pmc_log("blackbox", "  Mercenaries 2: World in Flames");
    pmc_log("blackbox", "  PMC Blackbox v%s (ASI Loader)", PMC_BLACKBOX_VERSION);
    pmc_log("blackbox", "============================================");
    pmc_log("blackbox", "  PID: %lu", (unsigned long)GetCurrentProcessId());
#ifndef PMC_DISABLE_SECUROM_EVENT
    pmc_log("blackbox", "  SecuROM event: created (signaled)");
#else
    pmc_log("blackbox", "  SecuROM event: skipped (no-crack build; licensed copy)");
#endif
    pmc_log("blackbox", "============================================");
}

#else /* PMC_DISABLE_LOG_STACK */

/* Inert stubs for the quiet variants. Exported so the plugin-facing ABI is
 * identical across all six (see "Build variants" at the top): there is no
 * console, no pmc_blackbox.log and no lock behind these, so a call formats
 * nothing and returns. Arguments are still evaluated by the caller, which is
 * why call sites must not hide side effects in their format arguments. */
__declspec(dllexport) void pmc_log(const char *source, const char *fmt, ...) {
    (void)source; (void)fmt;
}

__declspec(dllexport) void pmc_log_flush(void) { }

#endif /* PMC_DISABLE_LOG_STACK */

/* --- Underground spawn fix --- */

#define SPAWN_FLAG_VA  0x00DFBD74
#define SPAWN_FIX_REAPPLY_DELAY_MS  5000
#define SPAWN_FIX_REAPPLY_COUNT     4
#define SPAWN_FIX_REAPPLY_INTERVAL  3000

static void WriteSpawnFlag(void) {
    BYTE* flag = (BYTE*)SPAWN_FLAG_VA;
    DWORD oldProtect;
    if (VirtualProtect(flag, 1, PAGE_READWRITE, &oldProtect)) {
        *flag = 0x01;
        VirtualProtect(flag, 1, oldProtect, &oldProtect);
    }
}

static void FixSpawnValidation(void) {
    WriteSpawnFlag();
    pmc_log("blackbox", "Spawn validation flag set (0x%08X = 0x01)", SPAWN_FLAG_VA);
}

/*
 * The game's initialization (MOVQ store at ~0x006CEEBA) zeroes the .data region
 * containing the spawn flag AFTER DllMain has already set it.  Re-apply the flag
 * on a background thread after a delay so it persists past that zeroing.
 */
static DWORD WINAPI SpawnFlagWatchdog(LPVOID param) {
    (void)param;
    Sleep(SPAWN_FIX_REAPPLY_DELAY_MS);
    for (int i = 0; i < SPAWN_FIX_REAPPLY_COUNT; i++) {
        BYTE current = *(volatile BYTE*)SPAWN_FLAG_VA;
        if (current != 0x01) {
            WriteSpawnFlag();
            pmc_log("blackbox", "Spawn flag re-applied (was 0x%02X, pass %d/%d)",
                    current, i + 1, SPAWN_FIX_REAPPLY_COUNT);
        }
        if (i < SPAWN_FIX_REAPPLY_COUNT - 1)
            Sleep(SPAWN_FIX_REAPPLY_INTERVAL);
    }
    return 0;
}

/* --- Coexisting with other ASI loaders ---
 *
 * We do not look for them. No DLL-name probing, no reading another loader's
 * config, no module-list sniffing — pmc_bb scans its four search paths and
 * loads what is there, and that is the whole policy.
 *
 * This is a deliberate reversal. Earlier builds detected dxwrapper (by
 * dxwrapper.dll's presence, then by parsing its ini) and stood down so the two
 * of us would not scan the same directories. It worked, but the mechanism does
 * not survive contact with the rest of the ecosystem or with antivirus:
 *
 *   - It cannot generalize. dxwrapper has one fixed filename; the Ultimate ASI
 *     Loader family is renamed to whatever the game already imports (dinput8,
 *     version, winmm, ...). Detecting those means shipping the canonical
 *     DLL-search-order-hijack name set, which is exactly the string feature
 *     that got v0.5.1 flagged as Trojan:Win32/Wacatac.B!ml. See "Antivirus" in
 *     the README.
 *   - Config-first does not rescue it. Ultimate ASI Loader defaults to
 *     LoadPlugins=1 and needs no ini at all, so "no config present" is its
 *     normal loading state rather than evidence of anything; and its ini may be
 *     named after the DLL (version.dll -> version.ini), so finding the config
 *     requires the DLL name anyway.
 *   - It was never load-bearing. The old code said so itself: a missed
 *     detection just means we load the plugins and the other loader's later
 *     LoadLibrary is a refcount bump. Standing down was an optimization, and
 *     its failure mode (stand down when nobody else is loading) is the bad one,
 *     because then nothing loads at all.
 *
 * So the contract is inverted. Rather than pmc_bb guessing about everyone else,
 * whoever owns plugin loading decides at INSTALL time by choosing the variant:
 * ship a build with the ASI loader compiled out and there is no second loader
 * to coordinate with, because the code is absent rather than merely idle.
 * modkit owns the install directory and makes that call; see the variant matrix
 * in the Makefile. Double-loading, when it does happen, is a refcount bump.
 */

/* The exe's own folder, with a trailing backslash. Empty on failure. */
static void GetExeDir(char *out /* MAX_PATH */) {
    char *sep;
    if (!GetModuleFileNameA(NULL, out, MAX_PATH)) { out[0] = '\0'; return; }
    sep = strrchr(out, '\\');
    if (sep) *(sep + 1) = '\0'; else out[0] = '\0';
}
/* --- ASI plugin loader --- */

static HINSTANCE g_hinstSelf = NULL;

/**
 * Case-insensitive check whether a filename should be skipped (self-load prevention).
 * Skips anything named pmc_bb* and the DLL's own module filename.
 *
 * The prefix test covers the variant filenames (pmc_bb_crack_asi.dll and the
 * rest) without naming them, so it survives a change to the variant set. The
 * module-filename test below is the one that actually has to be right: it is
 * what catches a copy renamed to something else entirely.
 */
static int IsSelfModule(const char *filename) {
    if (_strnicmp(filename, "pmc_bb", 6) == 0) return 1;

    char self_name[MAX_PATH];
    if (GetModuleFileNameA(g_hinstSelf, self_name, MAX_PATH)) {
        char *sep = strrchr(self_name, '\\');
        const char *self_base = sep ? sep + 1 : self_name;
        if (_stricmp(filename, self_base) == 0) return 1;
    }
    return 0;
}

/**
 * Load all .asi files from a single directory.
 * Returns the number of plugins attempted (loaded + failed).
 */
static int LoadASIsFromDirectory(const char *dir_path, const char *display_prefix,
                                 int *out_loaded, int *out_failed) {
    char search_path[MAX_PATH];
    char full_path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    int count = 0;

    wsprintfA(search_path, "%s*.asi", dir_path);
    hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (IsSelfModule(fd.cFileName)) continue;

        wsprintfA(full_path, "%s%s", dir_path, fd.cFileName);

        HMODULE hMod = LoadLibraryA(full_path);
        if (hMod) {
            pmc_log("blackbox", "  [LOADED] %s%s", display_prefix, fd.cFileName);
            (*out_loaded)++;
        } else {
            DWORD err = GetLastError();
            pmc_log("blackbox", "  [FAILED] %s%s (error: 0x%08lX)",
                       display_prefix, fd.cFileName, (unsigned long)err);
            (*out_failed)++;
        }
        count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    return count;
}

/**
 * Discover and load .asi plugins from the standard search paths:
 *   1. Game root (exe directory)
 *   2. scripts/
 *   3. plugins/
 *   4. update/
 *
 * This matches the Ultimate ASI Loader's search paths, so existing
 * configurations (scripts/global.ini, file layout) work unchanged.
 * xinput1_3.dll (or any other ASI loader proxy) can be removed entirely.
 *
 * All four paths are scanned unconditionally. We do not check whether another
 * loader is installed or what it is configured to do — see "Coexisting with
 * other ASI loaders" above for why that check was removed and why the overlap
 * it used to avoid is harmless.
 *
 * Returns the PMC_SELF_* state for the `LOADER self=` token. With this compiled
 * in the answer is always PMC_SELF_ENABLED; the constant is still routed
 * through so the DllMain call site stays identical whether or not the loader
 * was built at all.
 */
static int LoadASIPlugins(void) {
    char exe_dir[MAX_PATH];
    char sub_dir[MAX_PATH];
    int total = 0, loaded = 0, failed = 0;

    GetExeDir(exe_dir);

    pmc_log("blackbox", "[ASI Loader]");
    pmc_log("blackbox", "  Base: %s", exe_dir);

    /* 1. Game root */
    total += LoadASIsFromDirectory(exe_dir, "", &loaded, &failed);

    /* 2. scripts/ */
    wsprintfA(sub_dir, "%sscripts\\", exe_dir);
    total += LoadASIsFromDirectory(sub_dir, "scripts\\", &loaded, &failed);

    /* 3. plugins/ */
    wsprintfA(sub_dir, "%splugins\\", exe_dir);
    total += LoadASIsFromDirectory(sub_dir, "plugins\\", &loaded, &failed);

    /* 4. update/ */
    wsprintfA(sub_dir, "%supdate\\", exe_dir);
    total += LoadASIsFromDirectory(sub_dir, "update\\", &loaded, &failed);

    if (total == 0)
        pmc_log("blackbox", "  (no .asi plugins found)");
    pmc_log("blackbox", "  Summary: %d loaded, %d failed, %d total", loaded, failed, total);

    return PMC_SELF_ENABLED;
}


/* --- Exported function (ordinal #1) ---
 *
 * The patched EXE imports this DLL by ordinal #1. This function is
 * the target of that import. It doesn't need to do anything — the real work
 * happens in DllMain. But the export must exist for the import to resolve.
 */
__declspec(dllexport) int __stdcall BlackboxEntry(void) {
    return 1;
}

/* Whether the modkit asked for a verbose run. The Lua/engine log hook detours
 * the game's hot shared log stub and runs per-call stack resolution + formatting
 * on every funneled call, which is too costly for regular gameplay — so the hook
 * is NOT installed at all unless PMC_VERBOSE_LOG is set (the stub then stays a
 * no-op and costs nothing). Any value other than unset/empty/"0"/"false"/"no"
 * counts as enabled; the crash handler is unaffected. */
static BOOL VerboseLoggingRequested(void) {
    char val[16];
    DWORD n = GetEnvironmentVariableA("PMC_VERBOSE_LOG", val, sizeof(val));
    if (n == 0) return FALSE;          /* unset or empty */
    if (n >= sizeof(val)) return TRUE; /* set to a long value — treat as on */
    if (lstrcmpiA(val, "0") == 0) return FALSE;
    if (lstrcmpiA(val, "false") == 0) return FALSE;
    if (lstrcmpiA(val, "no") == 0) return FALSE;
    return TRUE;
}

/* --- DLL entry point --- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_hinstSelf = hinstDLL;

#ifndef PMC_DISABLE_SECUROM_EVENT
        /* The SecuROM event MUST be created before the Sitext stub checks it.
         * Absent from the logging-only build — see CreateSecuROMEvent above. */
        CreateSecuROMEvent();
#endif

        /* Debug console + pmc_blackbox.log — safe in DllMain for AllocConsole.
         * Must come before anything that logs; in a quiet build there is
         * nothing to initialize and pmc_log is already a stub. */
#ifndef PMC_DISABLE_LOG_STACK
        InitDebugConsole();
#endif

        /* Crash handler — install first so any later fault is recorded before
         * the process dies. */
#ifndef PMC_DISABLE_CRASH_HANDLER
        InstallCrashHandler();
#endif

        /* Lua/engine log hooking — captures the game's stripped-out log stream.
         * The hook detours the game's SHARED log stub (0x006D5640), through which
         * ~700 stripped subsystem log calls funnel. Every call that survives the
         * cheap pre-filter then pays per-call stack resolution (VirtualQuery
         * syscalls) + per-arg formatting BEFORE anything is (or isn't) emitted —
         * far too costly to leave on that hot stub during normal gameplay.
         *
         * So the ENTIRE hook is gated behind PMC_VERBOSE_LOG: unset = not
         * installed at all, the stub stays a no-op and costs nothing (the 3.0
         * default); set = installed for a full-capture diagnostic run. The crash
         * handler above is always armed regardless. */
#ifndef PMC_DISABLE_LUA_LOG_HOOK
        if (VerboseLoggingRequested()) {
            InstallLuaLogHook();
        } else {
            pmc_log("blackbox", "Lua log hook: OFF "
                    "(set PMC_VERBOSE_LOG=1 to capture the game log)");
        }
#else
        pmc_log("blackbox", "Lua log hook: DISABLED at build time");
#endif

        /* Fix underground spawn — early write + deferred watchdog thread.
         * Game init zeroes this flag; the watchdog re-applies it. */
        FixSpawnValidation();
        CreateThread(NULL, 0, SpawnFlagWatchdog, NULL, 0, NULL);

        /* Load all .asi plugins (replaces external ASI loader).
         *
         * PMC_DISABLE_ASI_LOADER builds everything else and leaves plugin
         * loading to whatever else is installed. The `LOADER self=disabled`
         * token below is what tells a log reader that happened — without it,
         * "no ASI records in this log" is indistinguishable from "the user has
         * no plugins", which is exactly the ambiguity this pair of lines
         * exists to remove. */
#ifndef PMC_DISABLE_ASI_LOADER
        int self_state = LoadASIPlugins();
#else
        int self_state = PMC_SELF_DISABLED;
        pmc_log("blackbox", "[ASI Loader] DISABLED at build time");
#endif

        /* Run identity. Both records are log output, so they go out with the
         * log-stack: a quiet variant writes no log for them to appear in, and
         * build_id.c is not linked into it at all. That is the deliberate
         * trade for a zero-footprint build — a crack_only or crack_asi run
         * leaves nothing behind for the modkit's debug bundle to collect. */
#ifndef PMC_DISABLE_LOG_STACK
        /* What we did about plugin loading. Touches nothing on disk, so it
         * stays here on the startup path where it is guaranteed to be written
         * even if the process dies moments later. */
        EmitLoaderIdentity(self_state);

        /* Content-fingerprint the exe, the sidecar DLLs, the loaded .asi
         * plugins and the WADs. Spawns a thread and returns immediately: the
         * hashing reads hundreds of megabytes, and doing that here would hold
         * the loader lock across every byte of it. */
        StartBuildFingerprint(hinstDLL);
#else
        (void)self_state;
#endif
    }
    return TRUE;
}
