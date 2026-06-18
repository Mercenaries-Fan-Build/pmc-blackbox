# Makefile for PMC Blackbox — SecuROM spoof + debug console + ASI loader
# Output: pmc_bb.dll — game's import table must reference this name.
#
# Cross-compiles from macOS/Linux using MinGW, or native on Windows with MSVC.
#
# Prerequisites:
#   macOS:   brew install mingw-w64
#   Ubuntu:  apt install gcc-mingw-w64-i686
#   Windows: Visual Studio with C++ Desktop workload (or MinGW-w64 i686)
#
# MinHook is compiled from the local minhook/ directory.

CC_MINGW    = i686-w64-mingw32-gcc
STRIP_MINGW = i686-w64-mingw32-strip
CC_MSVC     = cl

TARGET      = pmc_bb.dll
DEF         = pmc_blackbox.def

# --- MinHook paths (local copy) ---
MINHOOK_INC  = ./minhook
MINHOOK_SRC  = ./minhook

# --- Source files (trimmed public release) ---
SRCS_BB     = pmc_blackbox.c lua_log_hook.c crash_handler.c
SRCS_MH     = $(MINHOOK_SRC)/hook.c \
              $(MINHOOK_SRC)/buffer.c \
              $(MINHOOK_SRC)/trampoline.c \
              $(MINHOOK_SRC)/hde/hde32.c

SRCS_ALL    = $(SRCS_BB) $(SRCS_MH)

EXTRA_CFLAGS =
CFLAGS      = -O2 -Wall -Wno-unused-function -I$(MINHOOK_INC) -shared -Wl,--enable-stdcall-fixup $(EXTRA_CFLAGS)
LDFLAGS     = -lkernel32 -luser32

.PHONY: all clean mingw msvc help

all: mingw

mingw: $(SRCS_BB) $(DEF)
	$(CC_MINGW) $(CFLAGS) -o $(TARGET) $(SRCS_ALL) $(DEF) $(LDFLAGS)
	-$(STRIP_MINGW) $(TARGET) 2>/dev/null || strip $(TARGET)
	@echo "Built: $(TARGET) ($$(wc -c < $(TARGET)) bytes)"
	@echo "Place next to Mercenaries2.exe (no separate MinHook DLL needed)"

msvc: $(SRCS_BB) $(DEF)
	$(CC_MSVC) /LD /O2 /GS- /I$(MINHOOK_INC) $(SRCS_ALL) /link /DEF:$(DEF) /OUT:$(TARGET) kernel32.lib user32.lib
	@echo "Built: $(TARGET)"

clean:
	rm -f $(TARGET) pmc_blackbox.obj pmc_blackbox.exp pmc_blackbox.lib *.o

help:
	@echo "Usage:"
	@echo "  make mingw   — Cross-compile with MinGW (macOS/Linux/Windows)"
	@echo "  make msvc    — Compile with MSVC (Windows)"
	@echo "  make clean   — Remove build artifacts"
	@echo ""
	@echo "Output: pmc_bb.dll (~8-10 KB) [PMC Blackbox v1]"
	@echo "  Place next to Mercenaries2.exe."
	@echo "  Game import table must reference pmc_bb.dll (patch with mercs2-crack-game)."
	@echo "  Includes: SecuROM bypass, debug console, crash handler, Lua logging, ASI loader."
	@echo "  MinHook is compiled in — no separate DLL needed."
