#ifndef ASB_HOST_H
#define ASB_HOST_H

/*
 * appsandbox_host.dll: what the app shell (app\AppSandbox, WinUI) calls.
 * Everything here runs on the shell's UI thread, which must have a large
 * stack (the shell creates it with 8 MB: VmConfig/VmInstance are about
 * 600 KB each and live on it).
 */

#include <windows.h>
#include "ui_bridge.h"

#ifdef ASB_HOST_BUILDING
#define ASB_HOST_API __declspec(dllexport)
#else
#define ASB_HOST_API __declspec(dllimport)
#endif

/* asb_host_begin results. */
#define ASB_HOST_RUN             0   /* show the window and call asb_host_start */
#define ASB_HOST_RUN_FOR_VM      1   /* same, started by a VM shortcut: keep the window hidden */
#define ASB_HOST_HEADLESS        2   /* --headless: call asb_host_run_headless, no window */
#define ASB_HOST_HANDED_OVER     3   /* a running instance took the request: exit */
#define ASB_HOST_SERVICE_RUNNING 4   /* the headless service holds the lock (the user was told): exit */

/* First call of the process, before any window: process-wide settings, the
   data folder's access list, the command line and the single-instance lock. */
ASB_HOST_API int __stdcall asb_host_begin(void);

/* The headless daemon; returns its exit code when it stops. */
ASB_HOST_API int __stdcall asb_host_run_headless(void);

/* Start the host on this thread. shell_window owns the dialogs the host
   opens. on_message receives every message for the shell (ui_bridge.h);
   the last one is {"type":"exit"}: the host has shut down and the process
   should end. Returns 0 on failure. */
ASB_HOST_API int __stdcall asb_host_start(HWND shell_window, UiBridgeCallback on_message);

/* One JSON command from the shell: {"action": "...", ...} (the actions are
   listed in AGENTS.md section 7). It is queued and handled from the message
   loop, so this returns at once. {"action":"requestExit","fromClose":bool}
   asks the host to exit; the host decides with the user what happens to
   running VMs and answers with a "window" or "exit" message. */
ASB_HOST_API int __stdcall asb_host_command(const wchar_t *json);

#endif /* ASB_HOST_H */
