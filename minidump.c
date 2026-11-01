/**
 * minidump.c — see minidump.h.
 *
 * MiniDumpWriteDump runs on its own thread: it suspends every other thread, so it cannot be the one
 * that faulted, and a thread started at the crash would need the loader lock and a stack the process
 * may no longer have room for.
 */
#include "minidump.h"

#include <dbghelp.h>
#include <string.h>

extern void pmc_log(const char *source, const char *fmt, ...);
extern void pmc_log_flush(void);

/* Address space held back for dbghelp to allocate from. It comes out of the game's budget for the whole
 * run, so it stays small; dbghelp copies memory out through small buffers. */
#define RESERVE_BYTES   (8u * 1024 * 1024)
#define WRITER_STACK    (256u * 1024)
/* A bound on the wait, so a stuck write cannot hold a dying process open forever. */
#define WRITE_WAIT_MS   (5u * 60 * 1000)

typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

static MiniDumpWriteDumpFn  g_write;
static HANDLE               g_go, g_done;
static void                *g_reserve;
static EXCEPTION_POINTERS  *g_ep;
static DWORD                g_tid;
static volatile LONG        g_taken;

/* `<game folder>\pmc_crash_<date>_<time>.dmp`; 0 when the folder cannot be read. */
static int dump_path(char *out /* MAX_PATH */) {
    char dir[MAX_PATH];
    SYSTEMTIME st;
    char *sep;
    if (!GetModuleFileNameA(NULL, dir, MAX_PATH)) return 0;
    sep = strrchr(dir, '\\');
    if (!sep) return 0;
    sep[1] = 0;
    GetLocalTime(&st);
    if (lstrlenA(dir) + 40 >= MAX_PATH) return 0;
    wsprintfA(out, "%spmc_crash_%04d%02d%02d_%02d%02d%02d.dmp", dir, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    return 1;
}

/* Runs once the loader lock is free, because it loads a DLL. A crash before it finishes gets no dump. */
static int arm(void) {
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDumpFn write;

    if (!dbghelp) {
        pmc_log("crash", "Minidump requested but UNAVAILABLE: dbghelp.dll did not load, error %lu", GetLastError());
        return 0;
    }
    write = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (!write) {
        pmc_log("crash", "Minidump requested but UNAVAILABLE: this dbghelp.dll has no MiniDumpWriteDump");
        return 0;
    }
    g_reserve = VirtualAlloc(NULL, RESERVE_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    g_write = write;
    pmc_log("crash", "Minidump on a fatal crash: ARMED (about 1.5 GB in the game folder, %lu MiB held back for it)",
            (unsigned long)(g_reserve ? RESERVE_BYTES >> 20 : 0));
    return 1;
}

static DWORD WINAPI WriterThread(LPVOID param) {
    char path[MAX_PATH];
    HANDLE file;
    MINIDUMP_EXCEPTION_INFORMATION info;
    DWORD start;
    BOOL ok;
    (void)param;

    if (!arm()) return 0;

    WaitForSingleObject(g_go, INFINITE);
    if (g_reserve) VirtualFree(g_reserve, 0, MEM_RELEASE);

    if (!dump_path(path)) {
        pmc_log("crash", "  minidump NOT written: the game folder could not be read");
        SetEvent(g_done);
        return 0;
    }
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        pmc_log("crash", "  minidump NOT written: %s could not be created, error %lu", path, GetLastError());
        SetEvent(g_done);
        return 0;
    }

    pmc_log("crash", "  minidump: writing %s", path);
    pmc_log_flush();
    start = GetTickCount();
    info.ThreadId = g_tid;
    info.ExceptionPointers = g_ep;
    info.ClientPointers = FALSE;
    ok = g_write(GetCurrentProcess(), GetCurrentProcessId(), file,
                 (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules),
                 &info, NULL, NULL);
    if (ok) {
        LARGE_INTEGER size;
        size.QuadPart = 0;
        GetFileSizeEx(file, &size);
        pmc_log("crash", "  minidump: wrote %lu MiB in %lu ms", (unsigned long)(size.QuadPart >> 20),
                GetTickCount() - start);
    } else {
        pmc_log("crash", "  minidump NOT written: MiniDumpWriteDump failed, error 0x%08lX", GetLastError());
    }
    CloseHandle(file);
    pmc_log_flush();
    SetEvent(g_done);
    return 0;
}

void PmcMinidumpArm(void) {
    HANDLE thread;

    g_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_go || !g_done) {
        pmc_log("crash", "Minidump requested but UNAVAILABLE: CreateEvent failed, error %lu", GetLastError());
        return;
    }
    thread = CreateThread(NULL, WRITER_STACK, WriterThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        pmc_log("crash", "Minidump requested but UNAVAILABLE: CreateThread failed, error %lu", GetLastError());
        return;
    }
    CloseHandle(thread);
}

void PmcMinidumpWrite(EXCEPTION_POINTERS *ep) {
    if (!g_write) return;
    if (InterlockedExchange(&g_taken, 1)) return;
    g_ep = ep;
    g_tid = GetCurrentThreadId();
    SetEvent(g_go);
    if (WaitForSingleObject(g_done, WRITE_WAIT_MS) != WAIT_OBJECT_0)
        pmc_log("crash", "  minidump: gave up waiting after %u ms", WRITE_WAIT_MS);
}
