# mercs2-pmc-blackbox

**pmc_bb** — a lightweight DRM bypass + debug DLL for Mercenaries 2: World in Flames.

Published in [six variants](#build-variants) so you install only the parts you need. Each keeps its own filename; there is no default `pmc_bb.dll`.

## What it does

Each feature below belongs to one of three groups — **crack**, **asi**, **log** — and a given build contains some subset of them. See [Build variants](#build-variants) for which is which.

- **SecuROM v7 spoof** *(crack)*: Creates the required `v7_XXXX` event so the game can boot without retail media or SecuROM running.
- **Debug console** *(log)*: Allocates a Windows console window and logs all game output (including Lua prints).
- **Crash handler** *(log)*: Logs exception info, registers, and call stack on a crash for easier diagnostics.
- **Lua logging hooks** *(log)*: Captures the game's stripped-out log stream. **Off by default** — the hook detours the game's hot shared log stub and runs per-call stack resolution + formatting on every funneled call, too costly for regular gameplay. Set `PMC_VERBOSE_LOG=1` to install it for a diagnostic run (every line + `@script:line`, plus world-load milestones under the `world` source). Output goes to the console + `pmc_blackbox.log`.
- **ASI loader** *(asi)*: Discovers and loads all `.asi` plugins from `scripts/`, `plugins/`, `update/`, and the game root — replacing the need for a separate ASI loader DLL.
- **dxwrapper aware** *(asi)*: If [dxwrapper](https://github.com/elishacloud/dxwrapper) is installed next to the exe with `[Plugins] LoadPlugins = 1`, it already loads `.asi` plugins itself. Rather than run two loaders over the same files, pmc_bb stands down and lets it own that job, still loading only the paths dxwrapper does not scan (`update/`, plus the game root when its `LoadFromScriptsOnly = 1`). Standing down affects plugin loading only: whatever else the variant contains — SecuROM spoof, console, crash handler, hooks, `pmc_log` — is unaffected and has no dxwrapper equivalent. No configuration needed on our side; it reads dxwrapper's own ini (`dxwrapper-<process>.ini`, falling back to `dxwrapper.ini`).
- **Run identity** *(log)*: At startup the log records *which* ASI loader is live and a content fingerprint of every artifact that actually loaded — see "Run identity in the log" below.
- **`pmc_log` export** *(always present)*: The logging API plugins resolve via `GetProcAddress`. Exported by every variant, including the ones with no log-stack, where it is an inert stub — see [Exports](#exports).

## Build variants

Three independent features, six published combinations:

| Asset | crack | asi | log | Use it when |
|---|:-:|:-:|:-:|---|
| `pmc_bb_fully_loaded.dll` | ✅ | ✅ | ✅ | Cracked copy, want everything. The default choice. |
| `pmc_bb_crack_only.dll` | ✅ | — | — | Cracked copy, another loader handles ASI, no diagnostics wanted. |
| `pmc_bb_crack_asi.dll` | ✅ | ✅ | — | Cracked copy with plugins, but no console or log file. |
| `pmc_bb_crack_log.dll` | ✅ | — | ✅ | Cracked copy, diagnostics on, another loader owns ASI. |
| `pmc_bb_asi_log.dll` | — | ✅ | ✅ | **Licensed copy.** Plugins + diagnostics, no DRM spoof. |
| `pmc_bb_log_only.dll` | — | — | ✅ | **Licensed copy.** Pure diagnostics; another loader owns ASI. |

- **crack** — the SecuROM v7 event spoof.
- **asi** — the ASI loader and dxwrapper interop.
- **log** — the console, `pmc_blackbox.log`, the `pmc_log` transport, the crash handler, the Lua hooks, and the `BUILD`/`LOADER` run-identity records.

There is deliberately **no asi-only build**: plenty of other loaders already do that job, and a build offering nothing beyond it has no reason to be chosen over them.

### Things worth knowing before you pick one

**Each variant keeps its own filename.** There is no required install name and nothing in the DLL depends on one: the `.def` carries no `LIBRARY` line, so each binary's export directory is stamped with its own filename and self-describes. The exe patcher writes whatever name the file has into the import table and binds `BlackboxEntry` by ordinal #1; the no-crack variants are loaded by path through dxwrapper's `LoadCustomDllPath`. Keeping the names distinct is also what lets the `BUILD` record for this DLL name the variant outright, so a log identifies the build that produced it.

**The two builds without the log-stack leave no trace of themselves.** No console, no `pmc_blackbox.log`, and therefore no `BUILD` records and no `LOADER` line. That is the point of a zero-footprint build, but it has a cost: for `crack_only` and `crack_asi` there is nothing for the modkit's debug bundle to collect, and no `BUILD` record naming the variant — because there is no log at all. If you are asking a user to send diagnostics, they need one of the four log builds.

**Dropping the log-stack is what actually shrinks the binary.** It removes `crash_handler.c`, `lua_log_hook.c`, `build_id.c`, `sha256.c` and all of MinHook from the link — roughly 46 KB down to 11 KB. Dropping crack or asi only removes their own code, which is small by comparison.

## Run identity in the log

Present in the four **log** variants only.

Two families of `[blackbox]` lines exist so a log can be tied to the setup that produced it. Both are written by the process, because **nothing outside the process can answer either question**: the exe on disk is not necessarily the exe that ran, and which ASI loader is active depends on a config file rather than on any binary.

### `BUILD` records — what actually loaded

```
BUILD <kind>=<name> <sha256|qsha256>=<hex-or-UNREADABLE> size=<decimal>
```

`kind` is `exe`, `dll`, `asi` or `wad`. Emitted for the **running** executable (`GetModuleFileNameA(NULL)`, not "the exe in the folder"), this DLL by its own mapped path (which is how the record names the variant), the sidecar DLLs (`dxwrapper.dll`, `cruise.dll`, `binkw32.dll`) plus any other `pmc_bb*.dll` mapped in — a second variant loaded alongside is a misconfiguration worth seeing rather than hiding — every `.asi` **mapped into the process** — a plugin that failed to load did not run and is not recorded — and every `*.wad` beside the exe.

`name` is always a **basename, never a path**. `pmc_blackbox.log` is what the modkit's debug bundle zips and what users paste into chat, so a path written here has leaked regardless of what any consumer does with it.

An artifact that exists but cannot be read is reported with `UNREADABLE` in place of the digest rather than omitted, because a missing record is indistinguishable from a missing file.

The hashing runs on a background thread — a 264 MB patch WAD digested under the loader lock would stall every other thread's `LoadLibrary` for the duration. A second sweep some seconds later picks up plugins loaded by another loader (dxwrapper's own `DllMain` can run after ours) and emits only what the first pass did not already record.

The WAD set is the one the engine mounts *from*, not an observation of the mounts themselves: the engine has opened nothing yet when this runs. It is deliberately a superset — every language WAD appears even though one is selected — because a consumer looks records up by name, and a guessed subset would produce spurious mismatches.

### `qsha256` — the quick digest for huge files

`vz.wad` is ~2.5 GB and cannot be digested whole at startup. Files **strictly larger than 1 GiB** therefore report `qsha256` instead of `sha256`. The two are different functions over the same bytes and must never be compared to each other, which is why the hash type is on the line.

```
qsha256(F) = SHA-256( F[0 .. 8388608)  ||  F[len(F)-8388608 .. len(F))  ||  le64(len(F)) )
```

- head = the first **8388608** bytes (8 MiB), in file order
- tail = the last **8388608** bytes (8 MiB), in file order
- `le64` = the exact file length in bytes, unsigned, **little-endian**, 8 bytes
- fed to a single SHA-256 in exactly that order — no separators, no per-chunk length prefixes, no padding between chunks
- output is the plain SHA-256 digest as 64 lowercase hex characters

The 1 GiB threshold is more than twice `2 × 8 MiB`, so head and tail can never overlap.

> This definition is a **cross-repo agreement**, not a local detail. `loadprobe` names the hash type but documents it only as "head+tail+size, for >1GiB files"; anything wanting to reproduce a `qsha256` — the modkit comparing a deployed-WAD ledger against a log, most of all — has to compute it exactly as above. Changing any constant here invalidates every `qsha256` already recorded.

`make check` compiles the digest natively and runs it against the FIPS 180-4 vectors plus the `qsha256` construction. The DLL itself is 32-bit Windows and usually cannot be run on the machine that builds it, so the one component with a silent failure mode is kept in portable C (`sha256.c`) and tested on the host.

### `LOADER` — which ASI loader is live

One line, every value from a closed vocabulary:

```
LOADER active=<pmc_bb|dxwrapper|external|none> self=<enabled|stood_down|disabled> external=<none|multiple|NAME>
```

- **`active`** — who owns plugin loading in this process.
- **`self`** — pmc_bb's own loader: `enabled` (we scanned every search path), `stood_down` (dxwrapper owns the scan; we cover only what it does not), `disabled` (compiled out — the `crack_log` and `log_only` variants).
- **`external`** — a third-party proxy ASI loader sitting next to the exe (`dinput8`, `dsound`, `ddraw`, `d3d9`, `d3d11`, `winmm`, `version`, `xinput1_3`, `xlive`, `msacm32`; `multiple` when more than one). A DLL by these names normally lives in System32, so a copy beside the exe is the signal.

This exists because the question cannot be answered from the binary. A DRM-free `v1.1 patched` build and a stock SecuROM build have the same import shape yet need different answers; dxwrapper's `LoadPlugins` moves ownership from a config file at runtime; our own loader can be compiled out; and a proxy loader the toolchain does not manage can be present. Only the process knows.

## Installation

1. Pick a variant from the [table above](#build-variants) and download it from the [latest GitHub Release](https://github.com/Mercenaries-Fan-Build/pmc-blackbox/releases), or build it (see "Building from source" below).
2. Copy it next to your `Mercenaries2.exe`, keeping its filename. No rename is needed — nothing requires a particular name.
3. For a **crack** variant, use [mercs2-crack-game](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) to patch the EXE and inject that filename into the import table. The game should now boot without SecuROM.
   For a **licensed** variant (`asi_log`, `log_only`), leave the exe untouched and point dxwrapper's `LoadCustomDllPath` at the file. Your real SecuROM activation satisfies the stock exe; these builds do not contain the spoof and must never fabricate the auth event.

Because the filename is preserved, the `BUILD` record for this DLL names the variant directly, so a log says which build produced it. The two variants without the log-stack write no log and so say nothing.

## Building from source

### Prerequisites

- **macOS:** `brew install mingw-w64`
- **Ubuntu/Debian:** `apt install gcc-mingw-w64-i686`
- **Windows:** Visual Studio with C++ Desktop workload (or MinGW-w64 i686)

### Build

```bash
make all                       # Cross-compile all six variants with MinGW
make pmc_bb_crack_log.dll      # ...or just one
make msvc                      # All six with MSVC (Windows; not reproducible)

make check                     # Host-native test of the SHA-256 / qsha256 digest
make help                      # The variant matrix, from the Makefile
```

Output: the six `pmc_bb_*.dll` assets — 11–13 KB for the quiet builds, 44–47 KB for the log builds.

The MinGW builds are reproducible: the Makefile zeroes the two build-time-varying PE fields, so identical source and the same `VERSION=` yield an identical hash. The MSVC path has no equivalent step and is for local Windows development only — CI ships the MinGW output.

## Exports

The export set is **identical in all six variants**, so a plugin that has found the module always finds the symbol, whichever build is installed. A varying export set would make `GetProcAddress("pmc_log")` succeed or fail depending on which asset the user picked — a difference the plugin cannot anticipate and cannot do anything useful about.

- **`BlackboxEntry` (ordinal #1):** The game's import table resolves this by ordinal. Callable but a no-op; real work happens in `DllMain`.
- **`pmc_log(source, fmt, ...)`:** Exported by name. ASI plugins resolve this at runtime via `GetProcAddress("pmc_log")` and use it for centralized logging. In the two variants without the log-stack it is an **inert stub** — the symbol resolves and the call returns, but there is no console and no log file behind it. Plugins brought in by some other loader call this even in builds whose own loader is compiled out, which is why it cannot simply be dropped.
- **`pmc_log_flush()`:** Explicit log flush for crash-survivable checkpoints. Also a stub in the quiet variants.

## Scope

This repository contains **only** the DLL and its build system. It does not include:

- Python patching tools (SecuROM removal, EXE injection) — see [mercs2-securom-bypass](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) for that.
- DLC compatibility patches or anim-table expansion — that belongs in a separate repo focused on DLC support.

## See also

- [mercs2-securom-bypass](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) — Crack tool to patch Mercenaries2.exe and inject the chosen crack variant into the import table.
