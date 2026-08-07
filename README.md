# mercs2-pmc-blackbox

**pmc_bb.dll** — a lightweight DRM bypass + debug DLL for Mercenaries 2: World in Flames.

## What it does

- **SecuROM v7 spoof:** Creates the required `v7_XXXX` event so the game can boot without retail media or SecuROM running.
- **Debug console:** Allocates a Windows console window and logs all game output (including Lua prints).
- **Crash handler:** Logs exception info, registers, and call stack on a crash for easier diagnostics.
- **Lua logging hooks:** Captures the game's stripped-out log stream. **Off by default** — the hook detours the game's hot shared log stub and runs per-call stack resolution + formatting on every funneled call, too costly for regular gameplay. Set `PMC_VERBOSE_LOG=1` to install it for a diagnostic run (every line + `@script:line`, plus world-load milestones under the `world` source). Output goes to the console + `pmc_blackbox.log`.
- **ASI loader:** Discovers and loads all `.asi` plugins from `scripts/`, `plugins/`, `update/`, and the game root — replacing the need for a separate ASI loader DLL.
- **dxwrapper aware:** If [dxwrapper](https://github.com/elishacloud/dxwrapper) is installed next to the exe with `[Plugins] LoadPlugins = 1`, it already loads `.asi` plugins itself. Rather than run two loaders over the same files, pmc_bb stands down and lets it own that job, still loading only the paths dxwrapper does not scan (`update/`, plus the game root when its `LoadFromScriptsOnly = 1`). Everything else here — SecuROM spoof, console, crash handler, hooks, `pmc_log` — is unaffected and has no dxwrapper equivalent. No configuration needed on our side; it reads dxwrapper's own ini (`dxwrapper-<process>.ini`, falling back to `dxwrapper.ini`).
- **Run identity:** At startup the log records *which* ASI loader is live and a content fingerprint of every artifact that actually loaded — see "Run identity in the log" below.

## Run identity in the log

Two families of `[blackbox]` lines exist so a log can be tied to the setup that produced it. Both are written by the process, because **nothing outside the process can answer either question**: the exe on disk is not necessarily the exe that ran, and which ASI loader is active depends on a config file rather than on any binary.

### `BUILD` records — what actually loaded

```
BUILD <kind>=<name> <sha256|qsha256>=<hex-or-UNREADABLE> size=<decimal>
```

`kind` is `exe`, `dll`, `asi` or `wad`. Emitted for the **running** executable (`GetModuleFileNameA(NULL)`, not "the exe in the folder"), the loader/sidecar DLLs (`pmc_bb.dll`, `dxwrapper.dll`, `cruise.dll`, `binkw32.dll`), every `.asi` **mapped into the process** — a plugin that failed to load did not run and is not recorded — and every `*.wad` beside the exe.

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
- **`self`** — pmc_bb's own loader: `enabled` (we scanned every search path), `stood_down` (dxwrapper owns the scan; we cover only what it does not), `disabled` (built with `-DPMC_DISABLE_ASI_LOADER`).
- **`external`** — a third-party proxy ASI loader sitting next to the exe (`dinput8`, `dsound`, `ddraw`, `d3d9`, `d3d11`, `winmm`, `version`, `xinput1_3`, `xlive`, `msacm32`; `multiple` when more than one). A DLL by these names normally lives in System32, so a copy beside the exe is the signal.

This exists because the question cannot be answered from the binary. A DRM-free `v1.1 patched` build and a stock SecuROM build have the same import shape yet need different answers; dxwrapper's `LoadPlugins` moves ownership from a config file at runtime; our own loader can be compiled out; and a proxy loader the toolchain does not manage can be present. Only the process knows.

## Installation

1. Build `pmc_bb.dll` (see "Building from source" below), or download it from the [latest GitHub Release](https://github.com/Mercenaries-Fan-Build/pmc-blackbox/releases).
2. Copy `pmc_bb.dll` next to your `Mercenaries2.exe`.
3. Use [mercs2-crack-game](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) to patch the EXE and inject this DLL into the import table.

The game should now boot without SecuROM.

## Building from source

### Prerequisites

- **macOS:** `brew install mingw-w64`
- **Ubuntu/Debian:** `apt install gcc-mingw-w64-i686`
- **Windows:** Visual Studio with C++ Desktop workload (or MinGW-w64 i686)

### Build

```bash
make mingw       # Cross-compile with MinGW
# or:
make msvc        # Compile with MSVC (Windows)

make check       # Host-native test of the SHA-256 / qsha256 digest
```

Output: `pmc_bb.dll` (~8–10 KB).

## Exports

- **`BlackboxEntry` (ordinal #1):** The game's import table resolves this by ordinal. Callable but a no-op; real work happens in `DllMain`.
- **`pmc_log(source, fmt, ...)`:** Exported by name. ASI plugins resolve this at runtime via `GetProcAddress("pmc_log")` and use it for centralized logging.
- **`pmc_log_flush()`:** Explicit log flush for crash-survivable checkpoints.

## Scope

This repository contains **only** the DLL and its build system. It does not include:

- Python patching tools (SecuROM removal, EXE injection) — see [mercs2-securom-bypass](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) for that.
- DLC compatibility patches or anim-table expansion — that belongs in a separate repo focused on DLC support.

## See also

- [mercs2-securom-bypass](https://github.com/Mercenaries-Fan-Build/mercs2-securom-bypass) — Crack tool to patch Mercenaries2.exe and inject pmc_bb.dll into the import table.
