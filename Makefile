# Makefile for PMC Blackbox — SecuROM spoof + debug console + ASI loader
#
# Cross-compiles from macOS/Linux using MinGW, or native on Windows with MSVC.
#
# Prerequisites:
#   macOS:   brew install mingw-w64
#   Ubuntu:  apt install gcc-mingw-w64-i686
#   Windows: Visual Studio with C++ Desktop workload (or MinGW-w64 i686)
#
# MinHook is compiled from the local minhook/ directory (log-stack builds only).
#
# ---------------------------------------------------------------------------
# Variant matrix
# ---------------------------------------------------------------------------
# Three independent features. Every published DLL is one subset of them:
#
#   crack   SecuROM v7 event spoof                    -DPMC_DISABLE_SECUROM_EVENT
#   asi     ASI loader, four search paths             -DPMC_DISABLE_ASI_LOADER
#   log     console, pmc_blackbox.log, pmc_log,       -DPMC_DISABLE_LOG_STACK
#           crash handler, Lua hooks, BUILD/LOADER
#
#   asset                      crack  asi  log
#   pmc_bb_fully_loaded.dll      Y     Y    Y
#   pmc_bb_crack_only.dll        Y     -    -
#   pmc_bb_crack_asi.dll         Y     Y    -
#   pmc_bb_crack_log.dll         Y     -    Y
#   pmc_bb_asi_log.dll           -     Y    Y
#   pmc_bb_log_only.dll          -     -    Y
#
# asi-only is deliberately absent: plenty of other loaders already do that job,
# and a build offering nothing else has no reason to be chosen over them.
#
# Each variant keeps its own filename — there is no default pmc_bb.dll and no
# required install name. The .def carries no LIBRARY line, so ld stamps the
# output filename into each export directory and every DLL self-describes. The
# crack variants are injected into the import table by the exe patcher, which
# writes whatever name the file has and binds BlackboxEntry by ordinal #1; the
# no-crack variants are loaded by path through dxwrapper's LoadCustomDllPath.
# Keeping the names distinct on disk is also what lets the BUILD record for
# this DLL name the variant outright, so a log identifies the build that
# produced it. The two variants without the log-stack still cannot say
# anything, by construction — they write no log.
#
# The features are independent in the build but NOT symmetric in what they
# imply: dropping the log-stack also drops crash_handler.c, lua_log_hook.c,
# build_id.c, sha256.c and all of MinHook from the link, so the quiet builds
# are substantially smaller. Dropping crack or asi only removes their own code.

CC_MINGW    = i686-w64-mingw32-gcc
STRIP_MINGW = i686-w64-mingw32-strip
CC_MSVC     = cl

DEF         = pmc_blackbox.def

VARIANTS = \
  pmc_bb_fully_loaded.dll \
  pmc_bb_crack_only.dll \
  pmc_bb_crack_asi.dll \
  pmc_bb_crack_log.dll \
  pmc_bb_asi_log.dll \
  pmc_bb_log_only.dll

# --- Version stamped into the DLL banner ---
# Defaults to the current git tag (e.g. v0.6.0, or v0.6.0-2-gabc123-dirty between
# tags). CI overrides it with the exact release tag: `make all VERSION=v0.6.0`.
# `?=` so an explicit `make VERSION=...` wins over the git-derived default.
VERSION    ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

# --- MinHook paths (local copy) ---
MINHOOK_INC  = ./minhook
MINHOOK_SRC  = ./minhook

# --- Source sets ---
# SRCS_QUIET is everything a build needs with the log-stack compiled out; the
# rest is pulled in only by the log variants. MinHook is here rather than in
# SRCS_QUIET because lua_log_hook.c is its only consumer.
SRCS_QUIET  = pmc_blackbox.c
SRCS_LOG    = lua_log_hook.c log_stub.c qm_fatal.c crash_handler.c fault_decode.c addr_space.c pool_stats.c sites.c \
              minidump.c build_id.c sha256.c \
              $(MINHOOK_SRC)/hook.c \
              $(MINHOOK_SRC)/buffer.c \
              $(MINHOOK_SRC)/trampoline.c \
              $(MINHOOK_SRC)/hde/hde32.c
SRCS_FULL   = $(SRCS_QUIET) $(SRCS_LOG)

# Everything a variant could depend on, so any source edit rebuilds all of them.
SRCS_ALL    = $(SRCS_FULL)

# Strip a leading "v" from the tag (v0.6.0 -> 0.6.0) so the banner "v%s" reads
# "v0.6.0" rather than "vv0.6.0".
VERSION_STR  = $(VERSION:v%=%)
VERSION_DEF  = -DPMC_BLACKBOX_VERSION='"$(VERSION_STR)"'
CFLAGS      = -O2 -Wall -Wno-unused-function -I$(MINHOOK_INC) -shared \
              -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp $(VERSION_DEF)
LDFLAGS     = -lkernel32 -luser32

# --- Per-variant feature flags and sources ---
# Target-specific variables, so the single build rule below stays generic.
pmc_bb_fully_loaded.dll: VARIANT_FLAGS =
pmc_bb_fully_loaded.dll: VARIANT_SRCS  = $(SRCS_FULL)

pmc_bb_crack_only.dll:   VARIANT_FLAGS = -DPMC_DISABLE_ASI_LOADER -DPMC_DISABLE_LOG_STACK
pmc_bb_crack_only.dll:   VARIANT_SRCS  = $(SRCS_QUIET)

pmc_bb_crack_asi.dll:    VARIANT_FLAGS = -DPMC_DISABLE_LOG_STACK
pmc_bb_crack_asi.dll:    VARIANT_SRCS  = $(SRCS_QUIET)

pmc_bb_crack_log.dll:    VARIANT_FLAGS = -DPMC_DISABLE_ASI_LOADER
pmc_bb_crack_log.dll:    VARIANT_SRCS  = $(SRCS_FULL)

pmc_bb_asi_log.dll:      VARIANT_FLAGS = -DPMC_DISABLE_SECUROM_EVENT
pmc_bb_asi_log.dll:      VARIANT_SRCS  = $(SRCS_FULL)

pmc_bb_log_only.dll:     VARIANT_FLAGS = -DPMC_DISABLE_SECUROM_EVENT -DPMC_DISABLE_ASI_LOADER
pmc_bb_log_only.dll:     VARIANT_SRCS  = $(SRCS_FULL)

# --- Host test ---
# The DLL is a 32-bit Windows binary and cannot be run on the machines it is
# usually built on, so the one piece with a silent failure mode — the SHA-256
# behind the `BUILD` records — is kept in portable C and tested natively here.
CC_HOST    ?= cc

.PHONY: all clean check check-log-stub check-sites check-pool check-sites-header mingw msvc help $(addsuffix .msvc,$(VARIANTS))

all: $(VARIANTS)
	@echo ""
	@echo "All six variants built at v$(VERSION_STR)."

# Back-compat alias for the old entry point, which built the single default DLL.
mingw: all

check: sha256.c test_sha256.c sha256.h log_stub.c test_log_stub.c log_stub.h \
       fault_decode.c test_fault_decode.c fault_decode.h addr_space.c test_addr_space.c addr_space.h \
       sites.c test_sites.c sites.h pmc_sites.gen.h pool_stats.c test_pool_stats.c pool_stats.h \
       qm_fatal.c test_qm_fatal.c qm_fatal.h
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_sha256_test sha256.c test_sha256.c
	/tmp/pmc_sha256_test
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_qm_fatal_test qm_fatal.c test_qm_fatal.c
	/tmp/pmc_qm_fatal_test
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_log_stub_test log_stub.c test_log_stub.c
	/tmp/pmc_log_stub_test
	$(CC_HOST) -O2 -Wall -Wextra -D_M_IX86 -o /tmp/pmc_fault_decode_test fault_decode.c test_fault_decode.c $(MINHOOK_SRC)/hde/hde32.c
	/tmp/pmc_fault_decode_test
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_addr_space_test addr_space.c test_addr_space.c
	/tmp/pmc_addr_space_test
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_sites_test sites.c test_sites.c
	/tmp/pmc_sites_test
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_pool_test pool_stats.c test_pool_stats.c
	/tmp/pmc_pool_test

# The site table against real executables, as <exe>[=<site>:<hex>,...] words:
#   make check-sites EXES="v11/Mercenaries2.exe=entity_vtable:BDB348 v10/Mercenaries2.exe"
check-sites: sites.c test_sites.c sites.h pmc_sites.gen.h
	@test -n "$(EXES)" || { echo "set EXES to executables"; exit 2; }
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_sites_test sites.c test_sites.c
	/tmp/pmc_sites_test $(EXES)

# The pool walk against captured minidumps, as <process.dmp> <texture_free> <texture_in_use> triples:
#   make check-pool DUMPS="menu/process.dmp 11697cc 1173fe8"
check-pool: pool_stats.c test_pool_stats.c pool_stats.h pmc_sites.gen.h
	@test -n "$(DUMPS)" || { echo "set DUMPS to <process.dmp> <texture_free> <texture_in_use> triples"; exit 2; }
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_pool_test pool_stats.c test_pool_stats.c
	/tmp/pmc_pool_test $(DUMPS)

# pmc_sites.gen.h against the table it is generated from, in a mercs2-wad-simulator checkout:
#   make check-sites-header WAD_SIMULATOR=../mercs2-wad-simulator
check-sites-header:
	@test -n "$(WAD_SIMULATOR)" || { echo "set WAD_SIMULATOR to a mercs2-wad-simulator checkout"; exit 2; }
	cd "$(WAD_SIMULATOR)" && cargo run -q -p mercs2_probe --bin pmc_sites_header > /tmp/pmc_sites.gen.h
	diff /tmp/pmc_sites.gen.h pmc_sites.gen.h

# The log-stub finder against real executables: pairs of <exe> <expected stub VA>, e.g.
#   make check-log-stub EXES="v11/Mercenaries2.exe 0x6AEF90 v10/Mercenaries2.exe 0x6D5640"
check-log-stub: log_stub.c test_log_stub.c log_stub.h
	@test -n "$(EXES)" || { echo "set EXES to <exe> <expected-va> pairs"; exit 2; }
	$(CC_HOST) -O2 -Wall -Wextra -o /tmp/pmc_log_stub_test log_stub.c test_log_stub.c
	/tmp/pmc_log_stub_test $(EXES)

$(VARIANTS): $(SRCS_ALL) $(DEF)
	$(CC_MINGW) $(CFLAGS) $(VARIANT_FLAGS) -o $@ $(VARIANT_SRCS) $(DEF) $(LDFLAGS)
	-$(STRIP_MINGW) $@ 2>/dev/null || strip $@
	@# Reproducible build: zero the two build-time-varying PE fields so identical
	@# source yields an identical hash (this ld ignores -Wl,--no-insert-timestamp).
	@# e_lfanew=0x80 → COFF TimeDateStamp @0x88, PE CheckSum @0xD8 (derived, and
	@# unverified for normal user-mode DLLs). Code bytes are untouched.
	@printf '\0\0\0\0' | dd of=$@ bs=1 seek=136 count=4 conv=notrunc 2>/dev/null
	@printf '\0\0\0\0' | dd of=$@ bs=1 seek=216 count=4 conv=notrunc 2>/dev/null
	@echo "Built: $@ v$(VERSION_STR) ($$(wc -c < $@) bytes) [$(if $(VARIANT_FLAGS),$(VARIANT_FLAGS),all features)]"

# The crash paths on a real Windows or Wine: see test_crash_win.c. Not a published artifact.
pmc_crash_test.exe: test_crash_win.c
	$(CC_MINGW) -O0 -Wall -o $@ test_crash_win.c

# --- MSVC (native Windows) ---
# Same matrix, same target-specific VARIANT_FLAGS/VARIANT_SRCS; `cl` has no
# equivalent of the PE-field zeroing above, so these are NOT reproducible and
# are not what CI ships. Use for local Windows development only.
msvc: $(addsuffix .msvc,$(VARIANTS))

$(addsuffix .msvc,$(VARIANTS)): %.msvc:
	$(CC_MSVC) /LD /O2 /GS- /I$(MINHOOK_INC) $(VERSION_DEF) $(VARIANT_FLAGS) \
	  $(VARIANT_SRCS) /link /DEF:$(DEF) /OUT:$* kernel32.lib user32.lib
	@echo "Built: $* v$(VERSION_STR) (MSVC, not reproducible)"

# The .msvc phonies need the same per-variant settings as their .dll namesakes.
pmc_bb_fully_loaded.dll.msvc: VARIANT_FLAGS =
pmc_bb_fully_loaded.dll.msvc: VARIANT_SRCS  = $(SRCS_FULL)
pmc_bb_crack_only.dll.msvc:   VARIANT_FLAGS = -DPMC_DISABLE_ASI_LOADER -DPMC_DISABLE_LOG_STACK
pmc_bb_crack_only.dll.msvc:   VARIANT_SRCS  = $(SRCS_QUIET)
pmc_bb_crack_asi.dll.msvc:    VARIANT_FLAGS = -DPMC_DISABLE_LOG_STACK
pmc_bb_crack_asi.dll.msvc:    VARIANT_SRCS  = $(SRCS_QUIET)
pmc_bb_crack_log.dll.msvc:    VARIANT_FLAGS = -DPMC_DISABLE_ASI_LOADER
pmc_bb_crack_log.dll.msvc:    VARIANT_SRCS  = $(SRCS_FULL)
pmc_bb_asi_log.dll.msvc:      VARIANT_FLAGS = -DPMC_DISABLE_SECUROM_EVENT
pmc_bb_asi_log.dll.msvc:      VARIANT_SRCS  = $(SRCS_FULL)
pmc_bb_log_only.dll.msvc:     VARIANT_FLAGS = -DPMC_DISABLE_SECUROM_EVENT -DPMC_DISABLE_ASI_LOADER
pmc_bb_log_only.dll.msvc:     VARIANT_SRCS  = $(SRCS_FULL)

clean:
	rm -f $(VARIANTS) pmc_bb.dll pmc_bb_log.dll pmc_crash_test.exe \
	      pmc_blackbox.obj pmc_blackbox.exp pmc_blackbox.lib *.o

help:
	@echo "Usage:"
	@echo "  make all      — Cross-compile all six variants with MinGW"
	@echo "  make <asset>  — Cross-compile one, e.g. make pmc_bb_crack_log.dll"
	@echo "  make msvc     — Compile all six with MSVC (Windows, not reproducible)"
	@echo "  make check    — Host-native tests: the SHA-256 behind the BUILD records, the log-stub finder"
	@echo "  make check-log-stub EXES=\"<exe> <va> ...\" — the log-stub finder against real executables"
	@echo "  make check-sites EXES=\"<exe>[=<site>:<hex>,...] ...\" — the site table against real executables"
	@echo "  make check-pool DUMPS=\"<dmp> <free> <in-use> ...\" — the pool walk against captured minidumps"
	@echo "  make check-sites-header WAD_SIMULATOR=<dir> — pmc_sites.gen.h against its generator"
	@echo "  make clean    — Remove build artifacts"
	@echo ""
	@echo "Variants (crack = SecuROM spoof, asi = ASI loader, log = log-stack):"
	@echo "  pmc_bb_fully_loaded.dll   crack + asi + log"
	@echo "  pmc_bb_crack_only.dll     crack"
	@echo "  pmc_bb_crack_asi.dll      crack + asi"
	@echo "  pmc_bb_crack_log.dll      crack + log"
	@echo "  pmc_bb_asi_log.dll        asi + log      (licensed copy)"
	@echo "  pmc_bb_log_only.dll       log            (licensed copy)"
	@echo ""
	@echo "All six install next to Mercenaries2.exe under their own filename."
	@echo "Cracked variants need the import table patched to reference that"
	@echo "filename (use mercs2-securom-bypass); the others are loaded by path"
	@echo "through dxwrapper's LoadCustomDllPath."
	@echo "MinHook is compiled in — no separate DLL needed."
