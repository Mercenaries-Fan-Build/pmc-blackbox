/**
 * build_id.c — bind this run to the exact bytes that produced it.
 *
 * Two things live here, and both exist because **only the process knows**:
 *
 *   1. `BUILD` records — a content fingerprint of every artifact that was
 *      actually loaded: the running executable, the loader/sidecar DLLs, every
 *      `.asi` in the module list, and the WADs beside the exe.
 *   2. The `LOADER` line — what THIS process did about loading plugins.
 *
 * ## Why the log has to carry this
 *
 * A crash report is worthless unless it can be tied to the setup that crashed.
 * Hashing the game folder after the fact answers a different question: the user
 * may have pulled the mod they suspect between the crash and the report, and
 * `Mercenaries2.exe` on disk is not necessarily the exe that ran — a cracked
 * sibling may have been launched instead, or the stock one directly, or via
 * Steam. `GetModuleFileNameA(NULL)` is the only source that cannot be wrong.
 *
 * Likewise our own loader: a DRM-free build and a stock SecuROM build are
 * indistinguishable by import table, dxwrapper's `[Plugins] LoadPlugins=1`
 * moves ownership of plugin loading at runtime, and our loader can be compiled
 * out. No static catalogue can answer "did WE load the plugins"; the process
 * can. Whether some third-party loader is installed is a question about the
 * directory, not about this process, and belongs to modkit — see
 * EmitLoaderIdentity.
 *
 * ## Record grammar — DO NOT DRIFT
 *
 * Consumed by `loadprobe::report::parse_build_line`
 * (mercs2-wad-simulator, crates/loadprobe/src/report.rs). A near miss does not
 * error, it silently yields an empty `build[]`.
 *
 *     BUILD <kind>=<name> <sha256|qsha256>=<hex-or-UNREADABLE> size=<decimal>
 *
 *   kind  ∈ exe | dll | asi | wad
 *   name  BASENAME ONLY — never a path (see "Paths" below)
 *   hex   64 lowercase hex chars, or the literal `UNREADABLE`
 *   size  decimal bytes; omitted entirely when the file could not be opened
 *
 * Logged under source `blackbox`, which loadprobe already filters on.
 *
 * ## Paths never appear
 *
 * Same discipline `resolve_addr` follows (crash_handler.c). `pmc_blackbox.log`
 * is what the modkit's debug bundle zips and what users paste into Discord, so
 * `C:\Users\<name>\...` written here has leaked no matter how careful any
 * downstream endpoint is. Every record carries a basename; full paths are used
 * only to open the file and are never formatted into a message.
 *
 * ## qsha256 — the quick digest for huge files
 *
 * `vz.wad` is ~2.5 GB. Digesting it whole at startup is not acceptable, so
 * files **strictly larger than 1 GiB** get `qsha256` instead. loadprobe
 * documents it as "head+tail+size" but defines no computation; this is the
 * definition, and modkit + loadprobe must reproduce it byte for byte:
 *
 *     qsha256(F) = SHA-256( F[0 .. 8MiB)  ||  F[len(F)-8MiB .. len(F))
 *                           ||  le64(len(F)) )
 *
 *   * head  = the first  8388608 bytes, in file order
 *   * tail  = the last   8388608 bytes, in file order
 *   * le64  = the exact file length in bytes, unsigned, little-endian, 8 bytes
 *   * fed to one SHA-256 in exactly that order, with NO separators, NO length
 *     prefixes on the chunks, and NO padding between them
 *   * the output is the plain SHA-256 digest, lowercase hex, 64 chars
 *
 * The threshold (> 1 GiB) is more than twice 2*8 MiB, so head and tail can
 * never overlap and the "two chunks" case needs no special handling.
 *
 * A file at or below the threshold is digested WHOLE and reported as `sha256`.
 * The two are different functions over the same file and must never be
 * compared to each other — that is why the hash type is on the wire.
 *
 * ## Threading
 *
 * `StartBuildFingerprint` only spawns a thread. A thread created in DllMain
 * does not begin executing until the loader lock is released, so none of the
 * file I/O below ever runs under it — which matters, because digesting a
 * 264 MB patch WAD while holding the loader lock would stall every other
 * thread's LoadLibrary for the duration.
 *
 * The thread does two passes. Pass 1 runs immediately and covers everything
 * visible at that point. Pass 2 runs after a delay and emits only `.asi` and
 * sidecar modules that were NOT already recorded — that is how a plugin loaded
 * by dxwrapper (whose own loader may run after ours) still gets fingerprinted
 * instead of silently missing from `build[]`.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include "build_id.h"
#include "sha256.h"

extern void pmc_log(const char *source, const char *fmt, ...);

/* ------------------------------------------------------------------ tuning */

/* Files strictly larger than this get qsha256 instead of a whole-file sha256.
 * Matches loadprobe's ">1GiB" documentation of the qsha256 hash type. */
#define QSHA_THRESHOLD   0x0000000040000000ULL   /* 1 GiB */
/* Head and tail chunk size. 2*8 MiB is well under the threshold, so the two
 * windows can never overlap. */
#define QSHA_CHUNK       0x00800000UL            /* 8 MiB */
/* Read granularity. Heap-allocated; never on the stack. */
#define IO_BUF           0x00100000UL            /* 1 MiB */
/* How long pass 2 waits for another loader's plugins to appear. Long enough to
 * cover a proxy DLL initializing after us, short enough to land well before a
 * world load. */
#define LATE_SWEEP_MS    8000

/* Buffer for a path COMPOSED from two parts (dir + name). Both halves can each
 * be MAX_PATH, so composing into a MAX_PATH buffer can overrun by a few bytes
 * on a deeply nested install. An over-long result simply fails to open and is
 * reported UNREADABLE, which is the right answer; smashing the stack is not. */
#define PATHBUF          (MAX_PATH * 2)

/* ------------------------------------------------------------- formatting */

/* wvsprintfA (what pmc_log formats with) supports no 64-bit conversion, so
 * sizes are rendered here and passed as %s. */
static const char *u64_dec(ULONGLONG v, char *out /* >= 21 */)
{
    char tmp[21];
    int i = 0, j = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return out; }
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10ULL)); v /= 10ULL; }
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
    return out;
}

/* Last path component. Never log anything but this. */
static const char *base_name(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    int ls = lstrlenA(s), lx = lstrlenA(suffix);
    return ls >= lx && _stricmp(s + ls - lx, suffix) == 0;
}

/* -------------------------------------------------------------- file hash */

/* Read exactly `want` bytes starting at `off`, feeding them to `c`.
 * Returns 0 on any short read or I/O error. */
static int digest_range(HANDLE h, pmc_sha256 *c, ULONGLONG off, ULONGLONG want,
                        unsigned char *buf)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    while (want) {
        DWORD chunk = (want > (ULONGLONG)IO_BUF) ? IO_BUF : (DWORD)want;
        DWORD got = 0;
        if (!ReadFile(h, buf, chunk, &got, NULL) || got == 0) return 0;
        pmc_sha256_update(c, buf, got);
        want -= got;
    }
    return 1;
}

/**
 * Digest one file. `hex` receives 64 lowercase hex chars or the literal
 * "UNREADABLE"; `*type` receives "sha256" or "qsha256"; `*have_size` is 0 only
 * when the file could not be opened or stat'ed at all.
 *
 * An unreadable file is REPORTED, not skipped: a record saying "this artifact
 * was here and we could not read it" is a fact a consumer can act on, whereas a
 * missing record is indistinguishable from the artifact not existing.
 */
static void hash_file(const char *path, char *hex /* >= 65 */,
                      const char **type, ULONGLONG *size, int *have_size)
{
    HANDLE h;
    LARGE_INTEGER li;
    unsigned char *buf;
    pmc_sha256 c;
    int ok = 0;

    *type = "sha256";
    *have_size = 0;
    *size = 0;
    lstrcpynA(hex, "UNREADABLE", 65);

    /* Share everything: the game may already hold vz.wad open. */
    h = CreateFileA(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return; }
    *size = (ULONGLONG)li.QuadPart;
    *have_size = 1;

    buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, IO_BUF);
    if (!buf) { CloseHandle(h); return; }

    pmc_sha256_init(&c);
    if (*size > QSHA_THRESHOLD) {
        /* qsha256: head || tail || le64(size). See the file header comment —
         * this definition is load-bearing for modkit and loadprobe. */
        unsigned char le[8];
        int i;
        *type = "qsha256";
        ok = digest_range(h, &c, 0, QSHA_CHUNK, buf)
          && digest_range(h, &c, *size - QSHA_CHUNK, QSHA_CHUNK, buf);
        if (ok) {
            for (i = 0; i < 8; i++)
                le[i] = (unsigned char)((*size >> (8 * i)) & 0xFF);
            pmc_sha256_update(&c, le, 8);
        }
    } else {
        ok = (*size == 0) ? 1 : digest_range(h, &c, 0, *size, buf);
    }

    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);

    if (ok) pmc_sha256_final_hex(&c, hex);
    else    lstrcpynA(hex, "UNREADABLE", 65);
}

/* ------------------------------------------------------- emission + dedupe */

/* Basenames already emitted, so pass 2 adds only what pass 1 could not see.
 * Only ever touched from the fingerprint thread, so no locking.
 *
 * SEEN_LEN is generous enough to hold any realistic basename in full. A
 * truncating compare would make two long names look like one and silently drop
 * a record, which is the failure mode this whole file exists to remove. */
#define SEEN_MAX 96
#define SEEN_LEN 128
static char g_seen[SEEN_MAX][SEEN_LEN];
static int  g_seenN;
static int  g_seenOverflow;

static int already_emitted(const char *name)
{
    int i;
    for (i = 0; i < g_seenN; i++)
        if (_stricmp(g_seen[i], name) == 0) return 1;
    return 0;
}

static void mark_emitted(const char *name)
{
    if (g_seenN < SEEN_MAX) {
        lstrcpynA(g_seen[g_seenN], name, SEEN_LEN);
        g_seenN++;
    } else {
        g_seenOverflow = 1;
    }
}

/**
 * Hash `path` and write one BUILD record for it under `kind`.
 * `name` is forced to a basename here so no caller can leak a path by mistake.
 * Returns 0 without emitting when this basename was already recorded.
 */
static int emit_artifact(const char *kind, const char *path)
{
    char hex[65], szbuf[21];
    const char *type;
    const char *name = base_name(path);
    ULONGLONG size;
    int have_size;

    if (!*name || already_emitted(name)) return 0;
    mark_emitted(name);

    hash_file(path, hex, &type, &size, &have_size);

    if (have_size)
        pmc_log("blackbox", "BUILD %s=%s %s=%s size=%s",
                kind, name, type, hex, u64_dec(size, szbuf));
    else
        pmc_log("blackbox", "BUILD %s=%s %s=%s", kind, name, type, hex);
    return 1;
}

/* ------------------------------------------------------ module enumeration */

/**
 * Walk this process's loaded modules, emitting a record for each one `want`
 * accepts. Uses the toolhelp snapshot rather than a PEB walk because this runs
 * off the loader lock, where the loader list can be mutated underneath a
 * hand-rolled walk. Module32First/Next live in kernel32, so no new import.
 */
static int scan_modules(int (*want)(const char *base), const char *kind)
{
    HANDLE snap;
    MODULEENTRY32 me;
    int tries, n = 0;

    for (tries = 0; tries < 4; tries++) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return 0;
        Sleep(25);
    }
    if (snap == INVALID_HANDLE_VALUE) return 0;

    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (want(me.szModule) && emit_artifact(kind, me.szExePath)) n++;
            me.dwSize = sizeof(me);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return n;
}

static int is_asi(const char *base) { return ends_with_ci(base, ".asi"); }

/* The loader/sidecar DLLs the toolchain manages. A module NOT matched here is
 * deliberately left out: enumerating every DLL in the process would sweep in
 * the user's GPU driver and overlay software, which is noise here and widens
 * what the log discloses about the machine.
 *
 * This list is used two ways — as a filter over the mapped modules, and as a
 * set of basenames to stat on disk for sidecars that have not loaded yet — so
 * it holds only names that are fixed. Our own DLL is not one of them: it ships
 * under six variant filenames, and is matched by is_pmc_bb below instead. */
static const char *SIDECAR_DLLS[] = {
    "dxwrapper.dll", "cruise.dll", "binkw32.dll"
};
#define SIDECAR_N ((int)(sizeof(SIDECAR_DLLS)/sizeof(SIDECAR_DLLS[0])))

/* Any pmc_bb build variant, by prefix. The running copy is already emitted by
 * its mapped path before this scan runs, so what this actually catches is a
 * SECOND variant mapped into the same process — a misconfiguration (two copies
 * installed under different names) that is worth seeing in the log rather than
 * filtering out. Prefix, not the six literal names, so the check does not have
 * to be revised every time the variant set changes. */
static int is_pmc_bb(const char *base)
{
    return _strnicmp(base, "pmc_bb", 6) == 0 && ends_with_ci(base, ".dll");
}

static int is_sidecar(const char *base)
{
    int i;
    if (is_pmc_bb(base)) return 1;
    for (i = 0; i < SIDECAR_N; i++)
        if (_stricmp(base, SIDECAR_DLLS[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------- WADs */

/* Mirror of modkit's `find_data_dir`: prefer `data\`, else the install root.
 * `out` gets a trailing backslash. */
static void wad_dir(const char *exe_dir, char *out /* PATHBUF */)
{
    DWORD a;
    wsprintfA(out, "%sdata\\", exe_dir);
    a = GetFileAttributesA(out);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return;
    lstrcpynA(out, exe_dir, PATHBUF);
}

/**
 * Fingerprint every `*.wad` beside the exe.
 *
 * This is the WAD set the engine mounts FROM, not an observation of the mounts
 * themselves — the engine has not opened anything yet when this runs, and it
 * exposes no mount log we could hook. It is deliberately a superset (all the
 * language WADs appear even though one is selected): a consumer comparing a
 * deployed-WAD ledger against this is looking up by name, and a superset makes
 * that lookup succeed where a guessed subset would produce a spurious mismatch.
 */
static int emit_wads(const char *exe_dir)
{
    char dir[PATHBUF], pattern[PATHBUF], full[PATHBUF];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    int n = 0;

    wad_dir(exe_dir, dir);
    wsprintfA(pattern, "%s*.wad", dir);
    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wsprintfA(full, "%s%s", dir, fd.cFileName);
        if (emit_artifact("wad", full)) n++;
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return n;
}

/* ------------------------------------------------------- the two passes */

static HINSTANCE g_self;

static void pass_one(void)
{
    char exe_path[MAX_PATH], exe_dir[MAX_PATH], full[PATHBUF];
    char *sep;
    int i;

    if (!GetModuleFileNameA(NULL, exe_path, MAX_PATH)) return;
    lstrcpynA(exe_dir, exe_path, MAX_PATH);
    sep = strrchr(exe_dir, '\\');
    if (sep) *(sep + 1) = '\0'; else exe_dir[0] = '\0';

    pmc_log("blackbox", "[Build identity]");

    /* The exe that is RUNNING. Not "the exe in the folder" — the modkit's
     * GameInfo.exe_path is documented as the base exe, "not necessarily the one
     * we launch", and launch_exe_path prefers the cracked sibling. Only this
     * call knows which one won. */
    emit_artifact("exe", exe_path);

    /* This DLL, by its mapped path: under dxwrapper's LoadCustomDllPath the
     * bytes that are running need not sit beside the exe at all. The basename
     * this yields is what names the build variant in the log. */
    if (g_self && GetModuleFileNameA(g_self, full, MAX_PATH))
        emit_artifact("dll", full);

    /* Sidecars that are already mapped — the mapped copy is the authority. */
    scan_modules(is_sidecar, "dll");

    /* ...then any that exist on disk but have not loaded yet (dxwrapper's own
     * DllMain can run after ours). Deduped by basename against the above. */
    for (i = 0; i < SIDECAR_N; i++) {
        wsprintfA(full, "%s%s", exe_dir, SIDECAR_DLLS[i]);
        if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES)
            emit_artifact("dll", full);
    }

    /* Every .asi actually mapped into the process — not every .asi on disk. A
     * plugin that failed to load did not run and must not be attributed. */
    scan_modules(is_asi, "asi");

    emit_wads(exe_dir);
}

/* Only ASIs and sidecars can appear late; the exe and the WADs cannot. */
static void pass_two(void)
{
    int n;
    /* Past SEEN_MAX the dedupe table stops recording, so a second pass would
     * re-emit artifacts pass 1 already covered — duplicate `build[]` entries
     * that a consumer would read as two different files. Say so and stop
     * rather than corrupt the set. */
    if (g_seenOverflow) {
        pmc_log("blackbox", "[Build identity] over %d artifacts — late sweep skipped "
                            "(records above the cap are absent, not duplicated)", SEEN_MAX);
        return;
    }
    n = scan_modules(is_sidecar, "dll") + scan_modules(is_asi, "asi");
    if (n)
        pmc_log("blackbox", "[Build identity] %d module(s) loaded after the first pass", n);
}

static DWORD WINAPI FingerprintThread(LPVOID param)
{
    (void)param;
    pass_one();
    Sleep(LATE_SWEEP_MS);
    pass_two();
    return 0;
}

void StartBuildFingerprint(HINSTANCE self)
{
    HANDLE t;
    g_self = self;
    t = CreateThread(NULL, 0, FingerprintThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ------------------------------------------------------- loader identity */

/**
 * `LOADER self=<enabled|disabled>` — what THIS process did about loading
 * plugins.
 *
 * The crash-reporting contract derives `game.loader` from the exe catalogue's
 * `ExeEntry.requires`, and that cannot work: a DRM-free `v1.1 patched` build
 * and a stock SecuROM build both carry `requires: null` yet need different
 * answers, and our loader can be compiled out of the shipped file. The import
 * table cannot express either. This line can, because it is written by the
 * process it describes.
 *
 * Scope: pmc_bb reports on ITSELF, and only on what it did. Detecting
 * third-party mod loaders is modkit's job — it owns the install directory and
 * can inspect it properly at deploy time, where we could only ever guess at it
 * from inside the process, and only by carrying the DLL names that got v0.5.1
 * flagged. See "Coexisting with other ASI loaders" in pmc_blackbox.c.
 *
 * That is why this line lost `active=` and `external=` in v0.6.0, along with
 * the `stood_down` value of `self`. All three described the relationship
 * between us and another loader, which is no longer something this process
 * knows or asks. What is left is the one fact it cannot be wrong about: did
 * this build scan for plugins. Modkit composes the ecosystem view from that
 * token plus its own scan.
 *
 * The value is a closed token: this becomes an enumerated wire field, and free
 * text there would violate the contract's "enumerated values only" rule.
 */
void EmitLoaderIdentity(int self_state)
{
    const char *self_tok;

    switch (self_state) {
        case PMC_SELF_DISABLED: self_tok = "disabled"; break;
        default:                self_tok = "enabled";  break;
    }

    pmc_log("blackbox", "LOADER self=%s", self_tok);
}
