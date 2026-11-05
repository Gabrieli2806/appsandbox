#ifndef UI_H
#define UI_H

#include <windows.h>

/* DLL export/import — matches asb_core.h */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Create and show the main application window.
   Returns the window handle, or NULL on failure. */
HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow);

/* Window class of the management window, and the registered message a second
   launch sends to it so the running instance shows itself instead of the new
   process failing with "already running". */
#define UI_MAIN_WINDOW_CLASS L"AppSandbox_Main"
#define UI_SHOW_MAIN_MESSAGE L"AppSandbox.ShowMainWindow"

/* Desktop / Start menu shortcuts run "AppSandbox.exe --vm "<name>"". A second
   launch hands the name to the running window with WM_COPYDATA (dwData =
   UI_COPYDATA_LAUNCH_VM); the window starts that VM, or brings its display
   to the front when it already runs. */
#define UI_COPYDATA_LAUNCH_VM 0x56425341u   /* "ASBV" */
void ui_launch_vm_by_name(const wchar_t *name);

/* Write a line to appsandbox.log and forward it to the registered
   AsbLogCallback (if any). Thread-safe: can be called from any thread.
   Implemented in asb_core.c (DLL). */
ASB_API void ui_log(const wchar_t *fmt, ...);

/* Get the HINSTANCE used to create the main window.
   Implemented in asb_core.c (DLL). */
ASB_API HINSTANCE ui_get_instance(void);

/* Save/load per-VM state JSON file (beside disk.vhdx).
   Implemented in asb_core.c (DLL). */
ASB_API void vm_save_state_json(const wchar_t *vhdx_path, BOOL install_complete);
ASB_API BOOL vm_load_state_json(const wchar_t *vhdx_path);

BOOL ui_command_active(void);
void ui_refresh_from_core(void);
BOOL ui_display_is_open(UINT64 id);
BOOL ui_display_open(UINT64 id);
void ui_display_close(UINT64 id);

#endif /* UI_H */
