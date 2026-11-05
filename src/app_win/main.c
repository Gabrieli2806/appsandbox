#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include "ui.h"
#include "headless.h"

/* Find the management window of an already running AppSandbox and ask it to
   show itself. The window may still be starting up, so retry briefly.
   Returns FALSE when no app window exists (the headless service holds the
   instance mutex instead). */
static BOOL show_running_instance(void)
{
    UINT show_msg = RegisterWindowMessageW(UI_SHOW_MAIN_MESSAGE);
    int attempt;

    if (!show_msg) return FALSE;
    for (attempt = 0; attempt < 20; attempt++) {
        HWND existing = FindWindowW(UI_MAIN_WINDOW_CLASS, NULL);
        if (existing) {
            DWORD pid = 0;
            DWORD_PTR result = 0;
            GetWindowThreadProcessId(existing, &pid);
            /* This process was just launched by the user, so it may hand the
               foreground to the running instance. */
            if (pid) AllowSetForegroundWindow(pid);
            if (SendMessageTimeoutW(existing, show_msg, 0, 0,
                                    SMTO_ABORTIFHUNG, 5000, &result))
                return TRUE;
        }
        Sleep(250);
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)pCmdLine;

    /* Per-monitor DPI awareness (Windows 10 1703+) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Initialize OLE/COM (required for WebView2) */
    OleInitialize(NULL);

    /* Headless daemon mode: appsandbox.exe --headless
       (normal launches pass no args, so the GUI path below is unaffected). */
    if (pCmdLine && wcsstr(pCmdLine, L"-headless")) {
        int rc = run_headless(hInstance, pCmdLine);
        OleUninitialize();
        return rc;
    }

    /* Single-instance: one AppSandbox (GUI or headless) at a time. The headless
       daemon (headless.c) acquires this SAME mutex, so GUI + headless are mutually
       exclusive -- required because both touch the one %ProgramData%\AppSandbox\
       vms.cfg (no file locking) and the machine-global HCN networks that asb_init
       wipes (hcn_cleanup_stale_networks). Global\ name = machine-wide (any folder),
       fixed string = path-independent. OS auto-releases on crash.
       The handle is intentionally held (leaked) for the process lifetime. */
    {
        HANDLE inst_mutex = CreateMutexW(NULL, FALSE, L"Global\\AppSandboxCoreHost");
        if (inst_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            /* The running instance may be the app hidden in the tray (closing
               the window while VMs run keeps it alive). Ask it to show its
               window instead of refusing to open. */
            if (show_running_instance()) {
                OleUninitialize();
                return 0;
            }
            MessageBoxW(NULL,
                L"AppSandbox is already running as the headless service. "
                L"Stop it before opening the app window.",
                L"App Sandbox", MB_ICONINFORMATION | MB_OK);
            OleUninitialize();
            return 0;
        }
    }

    /* Create main window */
    hwnd = ui_create_main_window(hInstance, nCmdShow);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", L"App Sandbox", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    /* Message loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return (int)msg.wParam;
}
