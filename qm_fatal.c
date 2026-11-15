/**
 * qm_fatal.c — the Quartermaster's no-dialog failure: a native message box, then exit (qm_fatal.h).
 *
 * The box is shown from a thread of its own, and the game thread that printed the line waits for it
 * without pumping messages. A message box on the game thread would run that thread's message loop
 * while the box is up, which delivers the game window's messages to its window procedure mid-print.
 * The game's windows are minimized first (asynchronously: their thread is the waiting one), so a
 * fullscreen window does not cover the box.
 */
#include "qm_fatal.h"

#include <string.h>

const char *QmFatalText(const char *line) {
    size_t n = sizeof(QM_FATAL_PREFIX) - 1;
    if (!line || strncmp(line, QM_FATAL_PREFIX, n) != 0 || line[n] == '\0') return NULL;
    return line + n;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* pmc_blackbox.c: pmc_log that waits for the log lock rather than dropping the line. */
extern void pmc_log_wait(const char *source, const char *fmt, ...);
extern void pmc_log_flush(void);

static volatile LONG g_stopping = 0;
static char g_text[2048];

static BOOL CALLBACK MinimizeOwnWindow(HWND hwnd, LPARAM pid) {
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == (DWORD)pid && IsWindowVisible(hwnd)) ShowWindowAsync(hwnd, SW_MINIMIZE);
    return TRUE;
}

#define BOX_HEAD "A mod failed, and the game has to stop.\r\n\r\n"
#define BOX_TAIL "\r\n\r\nThe same line is in pmc_blackbox.log."

static DWORD WINAPI BoxThread(LPVOID unused) {
    /* Assembled by hand: wsprintfA stops at 1024 characters, and the failure text can be longer. */
    char body[sizeof(BOX_HEAD) + sizeof(g_text) + sizeof(BOX_TAIL)];
    (void)unused;
    lstrcpyA(body, BOX_HEAD);
    lstrcatA(body, g_text);
    lstrcatA(body, BOX_TAIL);
    MessageBoxA(NULL, body, "Mercenaries 2 - Quartermaster", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

void QmFatalStop(const char *line) {
    HANDLE box;
    const char *text = QmFatalText(line);

    if (InterlockedCompareExchange(&g_stopping, 1, 0) != 0) {
        /* The first caller is showing the box and terminates the process when it closes. */
        for (;;) Sleep(INFINITE);
    }
    lstrcpynA(g_text, text ? text : line, (int)sizeof(g_text));
    pmc_log_wait("qm", "FATAL-NODIALOG: %s", g_text);
    pmc_log_wait("qm", "showing the failure in a message box; the game exits when it is closed");
    pmc_log_flush();

    EnumWindows(MinimizeOwnWindow, (LPARAM)GetCurrentProcessId());
    box = CreateThread(NULL, 0, BoxThread, NULL, 0, NULL);
    if (box) {
        WaitForSingleObject(box, INFINITE);
        CloseHandle(box);
        pmc_log_wait("qm", "message box closed: exiting with code %d", QM_FATAL_EXIT_CODE);
    } else {
        pmc_log_wait("qm", "the message box thread could not be created (error %lu): exiting with code %d",
                (unsigned long)GetLastError(), QM_FATAL_EXIT_CODE);
    }
    pmc_log_flush();
    TerminateProcess(GetCurrentProcess(), QM_FATAL_EXIT_CODE);
    for (;;) Sleep(INFINITE);
}
#endif
