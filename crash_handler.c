/**
 * crash_handler.c — capture the faulting site when the game dies.
 *
 * The retail EXE has no usable unhandled-exception path: on a fault the process
 * just vanishes, leaving no record of WHERE it crashed. This installs two nets so
 * any fatal fault is written to pmc_blackbox.log (source [crash]) before the
 * process goes down — the faulting EIP, the exception code, the AV target
 * address, the full integer register file, and the exe-range return addresses on
 * the stack (a poor-man's call stack).
 *
 *   - A Vectored Exception Handler catches the fault FIRST-chance (before any
 *     frame SEH), so it fires even if something later swallows the exception.
 *   - SetUnhandledExceptionFilter catches the classic last-chance fatal path.
 *
 * Safe by construction: faulting EIPs are de-duplicated (a repeating fault logs
 * once), a reentrancy guard prevents recursion if logging itself faults, and the
 * stack walk is skipped for stack-overflow exceptions (where touching the stack
 * would re-fault). Only "severe" codes are logged via the VEH so ordinary
 * first-chance noise is ignored.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

extern void pmc_log(const char *source, const char *fmt, ...);
extern void pmc_log_flush(void);

/* Set to `force` at the top of log_exception: 1 on the fatal/last-chance path,
 * 0 on a first-chance VEH hit. resolve_addr() consults it and skips the
 * loader-lock module APIs (GetModuleHandleExA/GetModuleFileNameA) entirely when
 * 0. A first-chance exception can fire on a thread that is contending for the
 * loader lock during world spawn/streaming; hammering those APIs there (up to
 * ~160x in the stack walk) perturbed the spawn and dropped the player through
 * the map into the pool. So first-chance stays as light as v0.4.1 (no loader
 * lock), and the rich module-resolved report runs only when the crash is
 * actually fatal. */
static int g_richResolve;

/* Resolve `addr` to human-readable "module.dll+0xOFFSET" form in `buf`. This is
 * what turns a bare faulting EIP (e.g. 6D982251) into "lua_trace.asi+0x2251" so
 * nobody has to hand-subtract the module base to find WHERE it died. Returns a
 * class the callers use to decide what to keep:
 *   0 = not in any loaded module (buf set to "")
 *   1 = application module  (Mercenaries2.exe, a .asi plugin, a game-dir dll)
 *   2 = Windows system module (ntdll, kernel32, an nvidia/d3d dll, ...)
 * Crash-safe: GetModuleHandleExA(FROM_ADDRESS|UNCHANGED_REFCOUNT) neither
 * allocates nor faults on a wild pointer and takes no reference we must free. */
static int resolve_addr(DWORD addr, char *buf, int buflen)
{
    HMODULE hmod = NULL;
    char path[MAX_PATH], low[MAX_PATH];
    const char *name = path, *p;
    DWORD n;
    int i, is_sys;
    buf[0] = 0;
    if (!g_richResolve) return 0;   /* first-chance: no loader-lock APIs (see g_richResolve) */
    if (addr < 0x00010000UL) return 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &hmod) || !hmod)
        return 0;
    n = GetModuleFileNameA(hmod, path, sizeof(path));
    if (!n) { wsprintfA(buf, "?+0x%lX", addr - (DWORD)hmod); return 1; }
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    for (i = 0; i < (int)n && i < MAX_PATH - 1; i++)
        low[i] = (path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i];
    low[i] = 0;
    is_sys = (strstr(low, "\\windows\\") != NULL);
    wsprintfA(buf, "%s+0x%lX", name, addr - (DWORD)hmod);
    return is_sys ? 2 : 1;
}

#define CRASH_SEEN_N    32
#define CRASH_REARM_MS  2000     /* re-log a recurring EIP after this gap */
static DWORD g_seen[CRASH_SEEN_N];
static DWORD g_seenTick[CRASH_SEEN_N];
static int   g_seenCount;

/* Handler ownership. A single in-handler flag conflated two different jobs:
 * per-thread REENTRANCY (the handler's own code faulted, must not recurse) and
 * process-wide EXCLUSION (two threads at once interleave, and pmc_log drops
 * lines outright when its lock is contended). One flag doing both meant a FATAL
 * crash on thread B was silently dropped while thread A sat in a first-chance
 * log — losing the killing blow, the one report that matters most.
 *
 * Track the OWNING thread id instead, which separates the two: reentry is
 * "owner == me", contention is "owner == someone else". A first-chance hit still
 * yields to a busy handler (it is throttled and survivable anyway); a fatal one
 * never gives up. */
static LONG  g_ownerTid;             /* tid inside log_exception; 0 = free */

/* How long a fatal report waits for another thread's handler to finish. Waiting
 * is what actually saves the report: pmc_log drops lines while its lock is held,
 * so barging in on a live handler would shred both. Bounded so a wedged holder
 * can never hang the crash path — a frozen process is worse than a torn log. */
#define FATAL_WAIT_MS   1000

/* Acquire the handler. Returns 0 = do not log, 1 = log (we own it),
 * 2 = log anyway (taken from a holder that would not finish). */
static int handler_enter(int force)
{
    LONG tid = (LONG)GetCurrentThreadId();
    DWORD start;

    if (g_ownerTid == tid)                                          /* logging itself faulted */
        return 0;
    if (InterlockedCompareExchange(&g_ownerTid, tid, 0) == 0)
        return 1;
    if (!force)                                                     /* first-chance yields */
        return 0;

    /* Fatal: the process is going down and this is the killing blow. Give the
     * holder a window to finish and release, then report regardless.
     * Time the wait off the clock, NOT off a count of Sleep(5) calls: the
     * scheduler tick is ~15.6ms, so counting nominal sleeps overshoots the
     * bound by ~3x and the "never hangs" guarantee stops holding. */
    start = GetTickCount();
    while (GetTickCount() - start < FATAL_WAIT_MS) {
        Sleep(5);
        if (InterlockedCompareExchange(&g_ownerTid, tid, 0) == 0)
            return 1;
    }
    InterlockedExchange(&g_ownerTid, tid);
    return 2;
}

static void handler_leave(void)
{
    InterlockedExchange(&g_ownerTid, 0);
}

/* Return 1 if this faulting EIP should be SUPPRESSED right now. A site is logged
 * on first sight and re-logged once CRASH_REARM_MS has elapsed since its last
 * log — so a fault that is HANDLED first-chance and then recurs FATALLY later is
 * not hidden forever (the old permanent dedup dropped the killing blow). The
 * fatal/last-chance path bypasses this entirely (see log_exception `force`). */
static int crash_suppress(DWORD addr)
{
    DWORD now = GetTickCount();
    int i;
    for (i = 0; i < g_seenCount; i++) {
        if (g_seen[i] == addr) {
            if (now - g_seenTick[i] < CRASH_REARM_MS)
                return 1;
            g_seenTick[i] = now;
            return 0;
        }
    }
    if (g_seenCount < CRASH_SEEN_N) {
        g_seen[g_seenCount]     = addr;
        g_seenTick[g_seenCount] = now;
        g_seenCount++;
    }
    return 0;
}

/* Human name for an exception code, so the header reads "ACCESS_VIOLATION"
 * instead of a bare C0000005 that has to be looked up. */
static const char *exc_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
    case 0xC0000409UL:                    return "STACK_BUFFER_OVERRUN";
    case 0xE06D7363UL:                    return "C++_EXCEPTION";
    default:                              return "unknown";
    }
}

static int is_severe(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case 0xC0000409UL:              /* __fastfail / stack-cookie */
        return 1;
    default:
        return 0;
    }
}

/* The engine Entity vtable (mercs2_annotations: Entity_Destruct @0x790170).
 * The world-load type-confusion crash (0x7939C0 / FUN_007938C0 entity dispatch)
 * derefs entity+0x188 — a handler/delegate that is non-NULL but ZEROED (its
 * +0x10 vtable is NULL). When any GP register at a crash points to an Entity
 * (its first dword == this vtable), post-mortem it below to NAME the culprit. */
#define ENTITY_VTABLE 0x00BDB410UL

/* How many bytes at `p` are safely committed+readable (0 if none). */
static DWORD safe_avail(DWORD p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x00010000UL) return 0;
    if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return 0;
    return ((DWORD)mbi.BaseAddress + (DWORD)mbi.RegionSize) - p;
}

/* Hexdump up to `n` bytes (dwords) at `p`, VirtualQuery-gated. */
static void dump_at(DWORD p, DWORD n, const char *label)
{
    DWORD avail = safe_avail(p), k;
    const DWORD *w = (const DWORD *)p;
    char line[256];
    int off;
    if (!avail) { pmc_log("crash", "  %s @%08lX: (unreadable)", label, p); return; }
    if (n > avail) n = avail;
    if (n > 96) n = 96;
    off = wsprintfA(line, "  %s @%08lX:", label, p);
    for (k = 0; k + 4 <= n; k += 4)
        off += wsprintfA(line + off, " %08lX", w[k / 4]);
    pmc_log("crash", "%s", line);
}

/* Scan `n` bytes at `p` for printable ASCII runs (>=4 chars) and log them —
 * surfaces entity/component name strings (e.g. "vz_...", "Road 0x...") that
 * identify which entity carries the corrupt handler. VirtualQuery-gated. */
static void scan_ascii(DWORD p, DWORD n, const char *label)
{
    DWORD avail = safe_avail(p), i, runlen = 0;
    const unsigned char *b = (const unsigned char *)p;
    char run[80];
    if (!avail) return;
    if (n > avail) n = avail;
    if (n > 512) n = 512;
    for (i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? b[i] : 0;
        if (c >= 0x20 && c < 0x7F && runlen < sizeof(run) - 1) {
            run[runlen++] = (char)c;
        } else {
            if (runlen >= 4) {
                run[runlen] = 0;
                pmc_log("crash", "  %s str@%08lX: \"%s\"", label, p + i - runlen, run);
            }
            runlen = 0;
        }
    }
}

/* Post-mortem an Entity (vtable 0xBDB410) at `ent`: dump its header, the
 * +0x188 handler (the crash target), the +0x28/+0xA0 component containers, and
 * scan all of them for name strings. Read-only, fully VQ-gated. */
static void dump_entity(DWORD ent)
{
    DWORD handler, cont28, contA0, descr;
    pmc_log("crash", "  --- ENTITY post-mortem @%08lX (vtable 0xBDB410) ---", ent);
    dump_at(ent + 0x00, 0x60, "ent+00");
    dump_at(ent + 0x180, 0x20, "ent+180");   /* covers +0x188 handler ptr */
    handler = (safe_avail(ent + 0x188) >= 4) ? *(const DWORD *)(ent + 0x188) : 0;
    pmc_log("crash", "  ent+0x188 (handler) = %08lX", handler);
    if (handler) {
        dump_at(handler, 0x40, "handler");
        scan_ascii(handler, 0x40, "handler");
    }
    cont28 = (safe_avail(ent + 0x28) >= 4) ? *(const DWORD *)(ent + 0x28) : 0;
    contA0 = (safe_avail(ent + 0xA0) >= 4) ? *(const DWORD *)(ent + 0xA0) : 0;
    descr  = (safe_avail(ent + 0x08) >= 4) ? *(const DWORD *)(ent + 0x08) : 0;
    if (cont28) dump_at(cont28, 0x60, "comp+28");
    if (contA0) dump_at(contA0, 0x60, "comp+A0");
    if (descr)  { dump_at(descr, 0x40, "descr+08"); scan_ascii(descr, 0x80, "descr"); }
    scan_ascii(ent, 0x200, "ent");
    if (cont28) scan_ascii(cont28, 0x100, "comp28");
    if (contA0) scan_ascii(contA0, 0x100, "compA0");
}

/* The 0x4AB26B world-population crash: a model-segment substructure walk.
 *   ecx = [param+0x1e0]+0x28 + idx*0x3c        (param @ [esp+0x1c])
 *   ebp = [ecx+4]                              (segment record, VALID)
 *   esi = [ebp+0x2c] + ecx_index               (+0x2c holds int 3, NOT a ptr)
 *   cmp word[esi+0xc] -> AV.
 * The faulting function is UNLABELED in Ghidra (0x4ab19a..0x4ab380 gap). param,
 * the model, and the segment are all reachable here read-only — name the asset
 * post-mortem instead of a (too-hot-to-set) breakpoint. ebp+0x28 carries a valid
 * pointer beside the bogus +0x2c=3, so dump its target too (suspected real array
 * / transposed {ptr,count} pair). */
#define SPAWN_WALK_LO 0x004AB1A0UL
#define SPAWN_WALK_HI 0x004AB380UL

static void dump_spawn_walk(CONTEXT *cx)
{
    DWORD esp = cx->Esp, ebp = cx->Ebp;
    DWORD param, model, ptr28;
    pmc_log("crash", "  --- 0x4AB26B model-segment spawn-walk post-mortem ---");
    /* param @ [esp+0x1c] (used by the fn as [param+0x1e0]) */
    param = (safe_avail(esp + 0x1c) >= 4) ? *(const DWORD *)(esp + 0x1c) : 0;
    pmc_log("crash", "  param([esp+1C]) = %08lX", param);
    if (param) {
        DWORD idx, contA0, rec;
        dump_at(param + 0x00, 0x40, "param+00");
        dump_at(param + 0x1c0, 0x60, "param+1C0");   /* covers +0x1e0 model + +0x1e4 index */
        scan_ascii(param, 0x200, "param");
        if (safe_avail(param) >= 4 && *(const DWORD *)param == ENTITY_VTABLE)
            dump_entity(param);
        /* component container @ +0xA0 (the 0x7E045E type-confusion site) */
        contA0 = (safe_avail(param + 0xA0) >= 4) ? *(const DWORD *)(param + 0xA0) : 0;
        pmc_log("crash", "  param+0xA0 (comp container) = %08lX", contA0);
        if (contA0) { dump_at(contA0, 0x40, "compA0"); scan_ascii(contA0, 0x80, "compA0"); }
        idx = (safe_avail(param + 0x1e4) >= 4) ? *(const DWORD *)(param + 0x1e4) : 0;
        model = (safe_avail(param + 0x1e0) >= 4) ? *(const DWORD *)(param + 0x1e0) : 0;
        pmc_log("crash", "  model([param+1E0]) = %08lX  idx([param+1E4]) = %08lX", model, idx);
        if (model) {
            dump_at(model + 0x00, 0x60, "model+00");
            scan_ascii(model, 0x200, "model");
            /* the engine record: rec = [model+0x28] base + idx*0x3c; ebp = [rec+4] */
            if (safe_avail(model + 0x28) >= 4) {
                DWORD base = *(const DWORD *)(model + 0x28);
                pmc_log("crash", "  model+0x28 (rec array base) = %08lX", base);
                if (base) {
                    rec = base + idx * 0x3c;
                    pmc_log("crash", "  rec = base + idx*0x3c = %08lX", rec);
                    dump_at(rec, 0x3c, "rec");
                }
            }
        }
    }
    /* segment record (ebp) + the valid +0x28 pointer next to the bad +0x2c=3 */
    dump_at(ebp, 0x40, "segm(ebp)");
    scan_ascii(ebp, 0x80, "segm");
    ptr28 = (safe_avail(ebp + 0x28) >= 4) ? *(const DWORD *)(ebp + 0x28) : 0;
    pmc_log("crash", "  segm+0x28 (valid ptr?) = %08lX   segm+0x2C (bad) = %08lX",
            ptr28, (safe_avail(ebp + 0x2c) >= 4) ? *(const DWORD *)(ebp + 0x2c) : 0);
    if (ptr28) { dump_at(ptr28, 0x60, "segm+28*"); scan_ascii(ptr28, 0x100, "segm+28*"); }
}

/* PRMG render-resource registry probe (the 0x47AA5C / 0x47A7D8 / 0x47A6FB family).
 * FUN_00478270 resolves two render-resource handles per primitive group from a
 * 256-slot parallel hash table: keys @0x197de48, handles @0x197da48 (sentinel
 * 0x197da44). A lookup MISS stores NULL (record+4=0 here); a stale/foreign handle
 * faults at [handle+0xfc] (record+0=0x08270420 here). This dumps what is actually
 * registered, reverse-resolves the faulting handle to its key, and classifies it
 * — so we can tell converter-mangled key vs unregistered resource vs stale handle.
 * Read-only, VirtualQuery-gated. */
#define PRMG_KEYTAB  0x0197DE48UL
#define PRMG_HNDTAB  0x0197DA48UL
static void dump_prmg_registry(CONTEXT *cx)
{
    DWORD elem = cx->Edi;          /* the 0x1c4-stride primitive-group element */
    DWORD rec0 = cx->Ebx;          /* record+0 handle (faulting deref [rec0+0xfc]) */
    DWORD rec4 = (safe_avail(elem + 8) >= 4) ? *(const DWORD *)(elem + 4) : 0xFFFFFFFFUL;
    const DWORD *keys = (const DWORD *)PRMG_KEYTAB;
    const DWORD *hnds = (const DWORD *)PRMG_HNDTAB;
    int i, nreg = 0, slot0 = -1;
    char line[256];
    int off;
    if (safe_avail(PRMG_KEYTAB) < 256 * 4 || safe_avail(PRMG_HNDTAB) < 256 * 4) {
        pmc_log("crash", "  [prmg-reg] tables unreadable");
        return;
    }
    for (i = 0; i < 256; i++) {
        if (keys[i] != 0) nreg++;
        if (rec0 != 0 && hnds[i] == rec0) slot0 = i;
    }
    pmc_log("crash", "  [prmg-reg] %d/256 keys registered; rec0=%08lX rec4=%08lX elem=%08lX",
            nreg, rec0, rec4, elem);
    if (slot0 >= 0)
        pmc_log("crash", "  [prmg-reg] rec0 handle = registry slot %d, KEY=%08lX (record+0 lookup key)",
                slot0, keys[slot0]);
    else
        pmc_log("crash", "  [prmg-reg] rec0=%08lX is NOT in the handle table (foreign/stale ptr)", rec0);
    dump_at(rec0, 0x20, "rec0-target");   /* what the faulting handle points at */
    /* Dump every registered (slot:key>handle) compactly so the full key set is
     * visible for offline rainbow-table resolution + base-vs-DLC diff. */
    off = 0; line[0] = 0;
    for (i = 0; i < 256; i++) {
        if (keys[i] == 0) continue;
        off += wsprintfA(line + off, " [%3d]%08lX>%08lX", i, keys[i], hnds[i]);
        if (off > 170) { pmc_log("crash", "  [prmg-reg]%s", line); off = 0; line[0] = 0; }
    }
    if (off) pmc_log("crash", "  [prmg-reg]%s", line);
    /* DISASM-VERIFIED record layout for the FUN_0047aa20 draw family: the faulting
     * deref is `cmp [ebx+0xfc],0` with EBX = record+4 (the NULL resource handle), and
     * the 0x1c4-stride record itself is at EBP (NOT EDI, which is arg0=the batch). So
     * the asset-identifying field is record+0 (the geometry descriptor, e.g. 0x08270420
     * here = loaded fine) — record+4 from the INFO sub-chunk is what resolved to NULL.
     * Dump the descriptor + record to name the binding-prop asset. */
    {
        DWORD rec = cx->Ebp;
        DWORD r0 = (safe_avail(rec) >= 4)     ? *(const DWORD *)rec       : 0;
        DWORD r4 = (safe_avail(rec + 8) >= 4) ? *(const DWORD *)(rec + 4) : 0;
        pmc_log("crash", "  [prmg-rec] record@EBP=%08lX record+0=%08lX record+4=%08lX (record+4=NULL handle)",
                rec, r0, r4);
        dump_at(r0, 0x80, "record+0 (geom descriptor)");  /* should carry the asset hash/name */
        dump_at(rec, 0x60, "record@EBP");                 /* the full 0x1c4 record head     */
        /* if the descriptor at record+0 carries a vtable, deref it one level for the name */
        if (r0 >= 0x00010000UL && safe_avail(r0) >= 4) {
            DWORD vt = *(const DWORD *)r0;
            dump_at(vt, 0x20, "record+0->vtable");
        }
    }
}

static void log_exception(EXCEPTION_POINTERS *ep, const char *via, int force)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    DWORD eip = (DWORD)(ULONG_PTR)er->ExceptionAddress;

    int owned = handler_enter(force);
    if (!owned)
        return;
    /* `force` (the fatal/last-chance path) ALWAYS logs; first-chance is throttled
     * per-EIP so a handled fault that later recurs fatally is still captured. */
    if (!force && crash_suppress(eip)) {
        handler_leave();
        return;
    }
    /* Gate loader-lock module resolution to the fatal path. When owned==2 a
     * first-chance handler is still running on another thread and will see this
     * flip to 1 — i.e. it may take the loader lock it was meant to avoid. That
     * trade only ever happens on the last-chance path, where the process is
     * terminating and there is no spawn left to perturb. */
    g_richResolve = force;

    {
        char eipmod[128];
        resolve_addr(eip, eipmod, sizeof(eipmod));
        pmc_log("crash", "==== %s EXCEPTION %08lX %s @ EIP=%08lX (%s) (flags=%lX) ====",
                via, er->ExceptionCode, exc_name(er->ExceptionCode), eip,
                eipmod[0] ? eipmod : "unknown module", er->ExceptionFlags);
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        DWORD kind = (DWORD)er->ExceptionInformation[0];
        DWORD tgt  = (DWORD)er->ExceptionInformation[1];
        const char *op = kind == 1 ? "WRITE" : kind == 8 ? "EXEC" : "READ";
        char tgtmod[128];
        const char *cls;
        resolve_addr(tgt, tgtmod, sizeof(tgtmod));
        pmc_log("crash", "  AV %s target=%08lX%s%s", op, tgt,
                tgtmod[0] ? "  " : "", tgtmod);
        /* Classify the target so the failure mode is obvious at a glance. */
        if (tgt < 0x00010000UL) {
            cls = "NULL page — a NULL/garbage pointer was dereferenced (base + small field offset)";
        } else {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)tgt, &mbi, sizeof(mbi)) != sizeof(mbi))
                cls = "invalid address";
            else if (mbi.State != MEM_COMMIT)
                cls = "unmapped/reserved memory (freed, or never allocated)";
            else if (kind == 1 && !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                cls = "write to read-only memory (const/.rdata or code)";
            else
                cls = "mapped but wrong — stale/type-confused pointer or out-of-range index";
        }
        pmc_log("crash", "  -> %s", cls);
        /* Register correlation: which register held the base pointer? Find the GP
         * register nearest the fault target within a plausible struct-field span,
         * so "target = ECX + 0x8" is spelled out instead of left to the eye. */
        {
            const DWORD regs[7] = { cx->Eax, cx->Ecx, cx->Edx, cx->Ebx,
                                    cx->Esi, cx->Edi, cx->Ebp };
            const char *names[7] = { "EAX", "ECX", "EDX", "EBX", "ESI", "EDI", "EBP" };
            int best = -1; long bestd = 0x7FFFFFFF; int r;
            for (r = 0; r < 7; r++) {
                long d = (long)(tgt - regs[r]);
                long ad = d < 0 ? -d : d;
                if (ad <= 0x4000 && ad < (bestd < 0 ? -bestd : bestd)) { best = r; bestd = d; }
            }
            if (best >= 0)
                pmc_log("crash", "  -> faulting pointer = %s(%08lX) %c 0x%lX%s",
                        names[best], regs[best], bestd < 0 ? '-' : '+',
                        bestd < 0 ? -bestd : bestd,
                        regs[best] < 0x00010000UL ? "   [base register is itself near-NULL]" : "");
        }
    }
    pmc_log("crash", "  EAX=%08lX ECX=%08lX EDX=%08lX EBX=%08lX",
            cx->Eax, cx->Ecx, cx->Edx, cx->Ebx);
    pmc_log("crash", "  ESP=%08lX EBP=%08lX ESI=%08lX EDI=%08lX",
            cx->Esp, cx->Ebp, cx->Esi, cx->Edi);

    /* Shallow stack walk: return addresses just above ESP that land in an
     * application module (the exe OR a loaded .asi / game-dir dll). Resolving via
     * the module table — instead of the old hardcoded 0x401000..0xC00000 exe
     * range — is what lets an ASI frame (e.g. lua_trace.asi+0x2251, loaded high at
     * 0x6D980000) actually show up; the old range silently dropped every such
     * frame. System-module frames (ntdll/kernel32/nvidia) are skipped as noise.
     * Skipped entirely on stack overflow, where reading the stack would re-fault
     * on the guard page. */
    if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        const DWORD *sp = (const DWORD *)cx->Esp;
        int i, found = 0;
        for (i = 0; i < 160 && found < 24; i++) {
            DWORD v = sp[i];
            char m[128];
            if (force) {
                if (resolve_addr(v, m, sizeof(m)) == 1) {
                    pmc_log("crash", "  stk+%03X = %08lX  %s", i * 4, v, m);
                    found++;
                }
            } else if (v >= 0x00401000UL && v < 0x00C00000UL) {
                /* first-chance: exe-range check only (v0.4.1) — no loader-lock resolve */
                pmc_log("crash", "  stk+%03X = %08lX", i * 4, v);
                found++;
            }
        }
    }

    /* Dump a window of memory at each GP register that looks like a heap/data
     * pointer. For the world-load texture-handle crash (AV reading the 0xF011157A
     * "texture" sentinel) the engine is walking a {hash, 0xF011157A, 0} record
     * array; dumping the structure at EDI/ESI/EAX/etc. captures the asset HASH
     * next to the sentinel, naming the exact unresolved texture post-mortem.
     * VirtualQuery-gated so a bad pointer never re-faults the handler. */
    {
        const DWORD regs[7] = { cx->Eax, cx->Ecx, cx->Edx, cx->Ebx,
                                cx->Esi, cx->Edi, cx->Ebp };
        const char *names[7] = { "EAX", "ECX", "EDX", "EBX", "ESI", "EDI", "EBP" };
        int r;
        for (r = 0; r < 7; r++) {
            DWORD p = regs[r];
            MEMORY_BASIC_INFORMATION mbi;
            if (p < 0x00010000UL)
                continue;
            if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
                continue;
            if (mbi.State != MEM_COMMIT)
                continue;
            if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                continue;
            /* clamp the 48-byte window to the committed region */
            {
                DWORD region_end = (DWORD)mbi.BaseAddress + (DWORD)mbi.RegionSize;
                DWORD avail = region_end - p;
                DWORD n = avail < 48 ? avail : 48;
                const DWORD *w = (const DWORD *)p;
                DWORD k;
                char line[256], m[128];
                int off = 0;
                off += wsprintfA(line + off, "  [%s=%08lX]", names[r], p);
                for (k = 0; k + 4 <= n && k < 48; k += 4)
                    off += wsprintfA(line + off, " %08lX", w[k / 4]);
                if (resolve_addr(p, m, sizeof(m)))
                    off += wsprintfA(line + off, "  (%s)", m);
                pmc_log("crash", "%s", line);
            }
        }
    }

    /* Entity post-mortem: find an Entity (first dword == 0xBDB410 vtable) in the
     * GP registers OR on the stack, and dump up to 3 distinct ones. The GlobalEnter
     * entity-component walk holds the owning entity `this` on the stack, so even
     * when the FAULTING object is a component (e.g. vtable 0xBAA0A0 / 0xA600D4 with
     * a NULL field), the entity that owns it is reachable here. Names the culprit
     * regardless of which leaf of the walk trips — no breakpoint / per-frame cost. */
    {
        DWORD seen[3];
        int n_seen = 0, k;
        const DWORD regs[6] = { cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi };
        int r, i;
        for (r = 0; r < 6 && n_seen < 3; r++) {
            DWORD p = regs[r];
            int dup = 0;
            if (safe_avail(p) < 4 || *(const DWORD *)p != ENTITY_VTABLE) continue;
            for (k = 0; k < n_seen; k++) if (seen[k] == p) dup = 1;
            if (!dup) { dump_entity(p); seen[n_seen++] = p; }
        }
        if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
            const DWORD *sp = (const DWORD *)cx->Esp;
            for (i = 0; i < 192 && n_seen < 3; i++) {
                DWORD v = sp[i];
                int dup = 0;
                if (v < 0x00010000UL || safe_avail(v) < 4) continue;
                if (*(const DWORD *)v != ENTITY_VTABLE) continue;
                for (k = 0; k < n_seen; k++) if (seen[k] == v) dup = 1;
                if (dup) continue;
                pmc_log("crash", "  (entity on stack @esp+%03X)", i * 4);
                dump_entity(v);
                seen[n_seen++] = v;
            }
        }
        if (n_seen == 0)
            pmc_log("crash", "  (no Entity vtable 0xBDB410 found in regs/stack)");
    }

    /* Targeted: the 0x4AB26B model-segment spawn-walk (param/model/segment not
     * reachable via the generic Entity scan since the regs are all small ints). */
    if (eip >= SPAWN_WALK_LO && eip < SPAWN_WALK_HI &&
        er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        dump_spawn_walk(cx);

    /* PRMG render-resource registry miss (FUN_0047a6c0 / FUN_0047aa20 family:
     * 0x47A6FB record+0 deref, 0x47AA5C / 0x47A7D8 record+4 / entity loop). */
    if (eip >= 0x0047A600UL && eip < 0x0047AC00UL &&
        er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        dump_prmg_registry(cx);

    pmc_log_flush();
    handler_leave();
}

static LONG CALLBACK VehHandler(EXCEPTION_POINTERS *ep)
{
    if (is_severe(ep->ExceptionRecord->ExceptionCode))
        log_exception(ep, "VEH", 0);    /* first-chance: throttled per EIP */
    return EXCEPTION_CONTINUE_SEARCH;   /* don't alter behavior — just record */
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS *ep)
{
    log_exception(ep, "UNHANDLED", 1);  /* fatal/last-chance: ALWAYS log */
    return EXCEPTION_EXECUTE_HANDLER;   /* terminate; we've recorded it */
}

void InstallCrashHandler(void)
{
    AddVectoredExceptionHandler(1, VehHandler);   /* 1 = first in the VEH chain */
    SetUnhandledExceptionFilter(UnhandledFilter);
    pmc_log("crash", "Crash handler armed (VEH + UnhandledExceptionFilter) — "
            "faults logged to source [crash].");
    pmc_log_flush();
}
