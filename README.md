# mercs2-pmc-blackbox

**pmc_bb.dll** — a lightweight DRM bypass + debug DLL for Mercenaries 2: World in Flames.

## What it does

- **SecuROM v7 spoof:** Creates the required `v7_XXXX` event so the game can boot without retail media or SecuROM running.
- **Debug console:** Allocates a Windows console window and logs all game output (including Lua prints).
- **Crash handler:** Logs exception info, registers, and call stack on a crash for easier diagnostics.
- **Lua logging hooks:** Captures the game's stripped-out log stream. **Off by default** — the hook detours the game's hot shared log stub and runs per-call stack resolution + formatting on every funneled call, too costly for regular gameplay. Set `PMC_VERBOSE_LOG=1` to install it for a diagnostic run (every line + `@script:line`, plus world-load milestones under the `world` source). Output goes to the console + `pmc_blackbox.log`.
- **ASI loader:** Discovers and loads all `.asi` plugins from `scripts/`, `plugins/`, `update/`, and the game root — replacing the need for a separate ASI loader DLL.

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
