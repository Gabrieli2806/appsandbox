#ifndef HEADLESS_H
#define HEADLESS_H

#include <windows.h>

/* Run the AppSandbox headless daemon (appsandbox.exe --headless).
   Owns the core for the process lifetime and serves a local HTTP API.
   Returns the process exit code. */
int run_headless(HINSTANCE hInst, const wchar_t *cmdline);

#define WM_ASB_API_REQUEST (WM_APP + 30)
#define WM_ASB_API_SHUTDOWN (WM_APP + 31)
BOOL asb_api_start(HINSTANCE instance, HWND owner);
void asb_api_stop(void);
LRESULT asb_api_dispatch(void *request);
BOOL asb_api_busy(void);

/* ---- For the MCP layer (mcp.c) ---- */

/* Make a REST call in-process, exactly as a client with the token would
   (same validation and behaviour), and get the JSON answer. method is one of
   ASB_API_*; body is the JSON request body or NULL (fewer than 8192
   characters). Waits while the app is busy with one of its own commands.
   On TRUE, *status is the HTTP status and *response the body (free() it).
   FALSE when the app is closing. Any thread except the window thread. */
#define ASB_API_GET    0
#define ASB_API_POST   1
#define ASB_API_PUT    2
#define ASB_API_DELETE 3
BOOL asb_api_call(int method, const wchar_t *path, const wchar_t *body, int *status, char **response);

/* Run fn(arg) where REST requests are handled (the window thread in the app),
   for the few things that have no REST route. Same threading as above. */
BOOL asb_api_run(void (*fn)(void *), void *arg);

void *asb_api_queue(void);                 /* the http.sys request queue (HANDLE) */
BOOL asb_api_stopping(void);
const char *asb_api_product_version(void); /* "1.2.3" */
DWORD asb_api_mcp_listen(BOOL on);
BYTE *asb_api_display_copy_frame(UINT64 vm_id, UINT *width, UINT *height, UINT *stride, int *why);
int asb_api_display_send_packets(UINT64 vm_id, const void *packets, int count);

#endif /* HEADLESS_H */
