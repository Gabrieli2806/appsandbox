/*
 * webview2_bridge.c — WebView2 hosting in pure C via COM vtables.
 *
 * Loads WebView2Loader.dll dynamically. Implements the COM callback
 * interfaces as plain C structs with vtables.
 *
 * The WebView lives in the elevated app, so it is locked to the app's own
 * page (web\index.html beside the exe): other navigations are cancelled,
 * new windows and downloads are refused, web messages from any other
 * document are ignored, and browser features are off (DevTools, browser
 * shortcut keys, autofill, external drops; the context menu keeps only the
 * editing commands). When WebView2 cannot start, or one of its processes
 * ends or hangs, the user is asked whether to reload the window instead of
 * being left with an empty one.
 */

#include "webview2_bridge.h"
#include "ui.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

/* We need the WebView2 COM interface definitions.
   The header uses MIDL_INTERFACE which expands to plain vtable structs in C. */
#include "WebView2.h"

/* ---- Dynamic loading of WebView2Loader.dll ---- */

typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

typedef HRESULT (STDAPICALLTYPE *PFN_GetAvailableCoreWebView2BrowserVersionString)(
    PCWSTR browserExecutableFolder,
    LPWSTR *versionInfo);

static PFN_CreateCoreWebView2EnvironmentWithOptions pfnCreateEnv;
static PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion;
static HMODULE g_wv2_module;

/* ---- Global state ---- */

static ICoreWebView2 *g_webview;
static ICoreWebView2Controller *g_controller;
static HWND g_parent_hwnd;
static HINSTANCE g_hInstance;
/* TRUE once the page has sent "uiReady" (its message listener exists);
   until then posts are queued. */
static BOOL g_ready;
/* Bumped by every start and by cleanup, so completions of an abandoned
   start are ignored. */
static LONG g_generation;
static BOOL g_shutting_down;

/* The only page the WebView may show: the full path of web\index.html, and
   (rarely) the URI WebView2 reported for it when that URI did not decode
   back to this path. */
static wchar_t g_page_path[MAX_PATH];
static LPWSTR g_page_uri;
/* Set right before this bridge navigates a new WebView to the page: the
   next main-frame navigation is that one. */
static BOOL g_expect_own_navigation;

/* Message queue for messages sent before the page is ready */
#define MAX_QUEUED 64
static wchar_t *g_queue[MAX_QUEUED];
static int g_queue_count;

/* ---- Failure reporting ---- */

/* Ordered by severity: a pending failure is only replaced by a worse one. */
enum {
    FAIL_NONE,
    FAIL_UNRESPONSIVE,      /* the page's render process hangs */
    FAIL_RENDERER_EXITED,   /* the page's render process ended */
    FAIL_START,             /* the environment or controller did not start */
    FAIL_BROWSER_EXITED     /* the WebView2 browser process ended */
};

#define ID_RELOAD 1001
/* After the user chose to wait for a hung page, do not ask again for this long. */
#define UNRESPONSIVE_QUIET_MS 60000

static int g_pending_failure;
static HRESULT g_pending_hr;
static UINT_PTR g_failure_timer;
static BOOL g_failure_prompt_open;
static ULONGLONG g_unresponsive_quiet_until;

/* The worst failure the user chose not to reload ("Not now"), other than a
   hung page. The window is unusable until it is reloaded, so the bridge
   asks again when the window is shown or restored, and F5 or Ctrl+R reload
   it. Cleared by every restart. */
static int g_declined_failure;
static HRESULT g_declined_hr;

/* The environment of the current WebView. A restart after a hung page
   makes a new controller in it, in the running browser process, instead
   of starting a new environment while that process shuts down. */
static ICoreWebView2Environment *g_env;

/* A restart the user asked for gets one automatic retry with a new
   environment, after RESTART_RETRY_MS: starting can fail while the
   previous browser process is still shutting down. */
#define RESTART_RETRY_MS 2000
static int g_restart_retries;

/* HRESULT_FROM_WIN32(ERROR_BUSY): the user data folder still belongs to a
   WebView2 browser process that has not finished (an App Sandbox window
   that was just closed, or one left behind by a process that was ended).
   Starting is retried every BUSY_RETRY_MS for BUSY_RETRIES tries, then the
   window uses a second user data folder so it still opens. */
#define WV2_E_BUSY       ((HRESULT)0x800700AAL)
#define BUSY_RETRY_MS    1000
#define BUSY_RETRIES     15
static int  g_busy_retries;
static BOOL g_alt_user_data;
static UINT_PTR g_restart_timer;

/* Subclass of the parent window: see parent_subclass_proc. */
#define PARENT_SUBCLASS_ID 1
static BOOL g_parent_subclassed;

static void report_failure(int kind, HRESULT hr);
static HRESULT start_environment(void);
static void start_failed(HRESULT hr);
static void schedule_key_reload(void);
static BOOL is_app_page_uri(const wchar_t *uri);

/* ---- Forward declarations ---- */

static WebView2MessageCallback g_message_callback;
static WebView2AcceleratorCallback g_accelerator_callback;

void webview2_set_message_callback(WebView2MessageCallback cb) {
    g_message_callback = cb;
}

void webview2_set_accelerator_callback(WebView2AcceleratorCallback cb) {
    g_accelerator_callback = cb;
}

/* Log a refused navigation, window, download or message. Capped so a page
   that keeps trying cannot flood the log. */
static void log_blocked(const wchar_t *what, const wchar_t *uri)
{
    static int logged;
    if (logged >= 20) return;
    logged++;
    ui_log(L"WebView2: blocked %s %.200s", what, uri ? uri : L"(unknown)");
}

/* IUnknown for a heap-allocated event handler. Name is a struct whose first
   members are lpVtbl and ref; Iface is the WebView2 handler interface. The
   object frees itself when the last reference goes. */
#define DEFINE_HANDLER_IUNKNOWN(Name, Iface)                                    \
    static HRESULT STDMETHODCALLTYPE Name##_QI(Iface *This, REFIID riid,       \
                                               void **ppv)                      \
    {                                                                           \
        if (IsEqualIID(riid, &IID_IUnknown) ||                                  \
            IsEqualIID(riid, &IID_##Iface)) {                                   \
            *ppv = This;                                                        \
            This->lpVtbl->AddRef(This);                                         \
            return S_OK;                                                        \
        }                                                                       \
        *ppv = NULL;                                                            \
        return E_NOINTERFACE;                                                   \
    }                                                                           \
    static ULONG STDMETHODCALLTYPE Name##_AddRef(Iface *This)                   \
    {                                                                           \
        return InterlockedIncrement(&((Name *)This)->ref);                      \
    }                                                                           \
    static ULONG STDMETHODCALLTYPE Name##_Release(Iface *This)                  \
    {                                                                           \
        LONG r = InterlockedDecrement(&((Name *)This)->ref);                    \
        if (r == 0) free(This);                                                 \
        return r;                                                               \
    }

/* ---- COM callback: Environment Created ---- */

typedef struct {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
    LONG ref;
    LONG generation;
} EnvHandler;

DEFINE_HANDLER_IUNKNOWN(EnvHandler, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)

/* Forward: controller handler creation */
static HRESULT STDMETHODCALLTYPE EnvH_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env);

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    EnvHandler_QI, EnvHandler_AddRef, EnvHandler_Release, EnvH_Invoke
};

/* ---- COM callback: Controller Created ---- */

typedef struct {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
    LONG ref;
    LONG generation;
} CtrlHandler;

DEFINE_HANDLER_IUNKNOWN(CtrlHandler, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)

static HRESULT STDMETHODCALLTYPE CtrlH_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller);

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    CtrlHandler_QI, CtrlHandler_AddRef, CtrlHandler_Release, CtrlH_Invoke
};

/* ---- COM callback: WebMessage Received ---- */

typedef struct {
    ICoreWebView2WebMessageReceivedEventHandlerVtbl *lpVtbl;
    LONG ref;
} MsgHandler;

DEFINE_HANDLER_IUNKNOWN(MsgHandler, ICoreWebView2WebMessageReceivedEventHandler)

static HRESULT STDMETHODCALLTYPE MsgH_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR json = NULL;
    LPWSTR source = NULL;
    wchar_t action[32];
    (void)This; (void)sender;

    /* Only the app's own page may drive the elevated host. */
    if (FAILED(args->lpVtbl->get_Source(args, &source)) || !is_app_page_uri(source)) {
        log_blocked(L"a message from", source);
        CoTaskMemFree(source);
        return S_OK;
    }
    CoTaskMemFree(source);

    if (SUCCEEDED(args->lpVtbl->get_WebMessageAsJson(args, &json)) && json) {
        /* "uiReady" means the page has registered its message listener:
           deliver what was queued while it loaded before anything the
           handler posts in reply. */
        if (json_get_string(json, L"action", action, ARRAYSIZE(action)) &&
            wcscmp(action, L"uiReady") == 0) {
            g_ready = TRUE;
            webview2_flush_queue();
        }
        if (g_message_callback)
            g_message_callback(json);
        CoTaskMemFree(json);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_msgVtbl = {
    MsgHandler_QI, MsgHandler_AddRef, MsgHandler_Release, MsgH_Invoke
};

/* ---- COM callback: Accelerator Key Pressed ---- */

typedef struct {
    ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl *lpVtbl;
    LONG ref;
} AcceleratorHandler;

DEFINE_HANDLER_IUNKNOWN(AcceleratorHandler, ICoreWebView2AcceleratorKeyPressedEventHandler)

/* Ctrl+Plus, Ctrl+Minus and Ctrl+0 zoom the page. The browser's own zoom
   keys are off along with its other shortcut keys, so they are handled here
   (Ctrl+wheel and pinch zoom keep working on their own). Returns TRUE when
   the key was a zoom key. */
static BOOL handle_zoom_key(ICoreWebView2Controller *controller, UINT key)
{
    static const double steps[] = {
        0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0,
        1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0
    };
    const int count = (int)(sizeof(steps) / sizeof(steps[0]));
    double zoom = 1.0;
    int i;

    if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0)
        return FALSE;
    switch (key) {
    case VK_OEM_PLUS: case VK_ADD:
    case VK_OEM_MINUS: case VK_SUBTRACT:
    case '0': case VK_NUMPAD0:
        break;
    default:
        return FALSE;
    }
    if (!controller || FAILED(controller->lpVtbl->get_ZoomFactor(controller, &zoom)))
        return TRUE;

    if (key == VK_OEM_PLUS || key == VK_ADD) {
        for (i = 0; i < count && steps[i] <= zoom + 0.001; i++) {}
        if (i < count) zoom = steps[i];
    } else if (key == VK_OEM_MINUS || key == VK_SUBTRACT) {
        for (i = count - 1; i >= 0 && steps[i] >= zoom - 0.001; i--) {}
        if (i >= 0) zoom = steps[i];
    } else {
        zoom = 1.0;
    }
    controller->lpVtbl->put_ZoomFactor(controller, zoom);
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE AcceleratorH_Invoke(
    ICoreWebView2AcceleratorKeyPressedEventHandler *This,
    ICoreWebView2Controller *sender,
    ICoreWebView2AcceleratorKeyPressedEventArgs *args)
{
    UINT key = 0;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    INT lparam = 0;
    (void)This;

    /* This event fires for every accelerator key, including the browser
       keys that AreBrowserAcceleratorKeysEnabled turned off. */
    if (SUCCEEDED(args->lpVtbl->get_VirtualKey(args, &key)) &&
        SUCCEEDED(args->lpVtbl->get_KeyEventKind(args, &kind)) &&
        SUCCEEDED(args->lpVtbl->get_KeyEventLParam(args, &lparam)) &&
        kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) {
        if (key == VK_F11) {
            args->lpVtbl->put_Handled(args, TRUE);
            if (!(lparam & (1 << 30)) && g_accelerator_callback)
                g_accelerator_callback(key);
        } else if (g_declined_failure != FAIL_NONE &&
                   (key == VK_F5 || (key == 'R' && GetKeyState(VK_CONTROL) < 0))) {
            /* The user declined to reload after a failure: F5 and Ctrl+R
               (otherwise off with the browser keys) reload the window. */
            args->lpVtbl->put_Handled(args, TRUE);
            if (!(lparam & (1 << 30)))
                schedule_key_reload();
        } else if (handle_zoom_key(sender, key)) {
            args->lpVtbl->put_Handled(args, TRUE);
        }
    }
    return S_OK;
}

static ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl g_acceleratorVtbl = {
    AcceleratorHandler_QI, AcceleratorHandler_AddRef, AcceleratorHandler_Release,
    AcceleratorH_Invoke
};

/* ---- COM callback: Navigation Starting (main frame and iframes) ---- */

typedef struct {
    ICoreWebView2NavigationStartingEventHandlerVtbl *lpVtbl;
    LONG ref;
    BOOL frame;     /* registered with add_FrameNavigationStarting */
} NavHandler;

DEFINE_HANDLER_IUNKNOWN(NavHandler, ICoreWebView2NavigationStartingEventHandler)

static HRESULT STDMETHODCALLTYPE NavHandler_Invoke(
    ICoreWebView2NavigationStartingEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args)
{
    BOOL frame = ((NavHandler *)This)->frame;
    BOOL own = FALSE;
    LPWSTR uri = NULL;
    (void)sender;

    if (FAILED(args->lpVtbl->get_Uri(args, &uri)) || !uri) {
        args->lpVtbl->put_Cancel(args, TRUE);
        return S_OK;
    }
    if (!frame && g_expect_own_navigation && _wcsnicmp(uri, L"file:", 5) == 0) {
        own = TRUE;
        g_expect_own_navigation = FALSE;
    }
    if (!is_app_page_uri(uri)) {
        if (!own) {
            log_blocked(frame ? L"a frame navigation to" : L"a navigation to", uri);
            args->lpVtbl->put_Cancel(args, TRUE);
            CoTaskMemFree(uri);
            return S_OK;
        }
        /* This is the navigation the bridge itself started, but WebView2
           spelled the page's URI in a form that does not decode back to
           the page's path. Allow that exact URI from now on. */
        ui_log(L"WebView2: the app page is reported as %.300s", uri);
        CoTaskMemFree(g_page_uri);
        g_page_uri = uri;
        uri = NULL;
    }
    /* A new document is loading: hold posts until it sends "uiReady". */
    if (!frame) g_ready = FALSE;
    CoTaskMemFree(uri);
    return S_OK;
}

static ICoreWebView2NavigationStartingEventHandlerVtbl g_navVtbl = {
    NavHandler_QI, NavHandler_AddRef, NavHandler_Release, NavHandler_Invoke
};

/* ---- COM callback: New Window Requested ---- */

typedef struct {
    ICoreWebView2NewWindowRequestedEventHandlerVtbl *lpVtbl;
    LONG ref;
} NewWindowHandler;

DEFINE_HANDLER_IUNKNOWN(NewWindowHandler, ICoreWebView2NewWindowRequestedEventHandler)

/* Handled without a NewWindow: window.open() returns null and nothing opens. */
static HRESULT STDMETHODCALLTYPE NewWindowHandler_Invoke(
    ICoreWebView2NewWindowRequestedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args)
{
    LPWSTR uri = NULL;
    (void)This; (void)sender;
    args->lpVtbl->put_Handled(args, TRUE);
    if (SUCCEEDED(args->lpVtbl->get_Uri(args, &uri)))
        log_blocked(L"a new window for", uri);
    CoTaskMemFree(uri);
    return S_OK;
}

static ICoreWebView2NewWindowRequestedEventHandlerVtbl g_newWindowVtbl = {
    NewWindowHandler_QI, NewWindowHandler_AddRef, NewWindowHandler_Release,
    NewWindowHandler_Invoke
};

/* ---- COM callback: Download Starting (ICoreWebView2_4) ---- */

typedef struct {
    ICoreWebView2DownloadStartingEventHandlerVtbl *lpVtbl;
    LONG ref;
} DownloadHandler;

DEFINE_HANDLER_IUNKNOWN(DownloadHandler, ICoreWebView2DownloadStartingEventHandler)

static HRESULT STDMETHODCALLTYPE DownloadHandler_Invoke(
    ICoreWebView2DownloadStartingEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2DownloadStartingEventArgs *args)
{
    ICoreWebView2DownloadOperation *download = NULL;
    LPWSTR uri = NULL;
    (void)This; (void)sender;
    args->lpVtbl->put_Cancel(args, TRUE);
    args->lpVtbl->put_Handled(args, TRUE);
    if (SUCCEEDED(args->lpVtbl->get_DownloadOperation(args, &download)) && download) {
        download->lpVtbl->get_Uri(download, &uri);
        download->lpVtbl->Release(download);
    }
    log_blocked(L"a download of", uri);
    CoTaskMemFree(uri);
    return S_OK;
}

static ICoreWebView2DownloadStartingEventHandlerVtbl g_downloadVtbl = {
    DownloadHandler_QI, DownloadHandler_AddRef, DownloadHandler_Release,
    DownloadHandler_Invoke
};

/* ---- COM callback: Context Menu Requested (ICoreWebView2_11) ---- */

typedef struct {
    ICoreWebView2ContextMenuRequestedEventHandlerVtbl *lpVtbl;
    LONG ref;
} MenuHandler;

DEFINE_HANDLER_IUNKNOWN(MenuHandler, ICoreWebView2ContextMenuRequestedEventHandler)

static BOOL is_editing_command(const wchar_t *name)
{
    static const wchar_t *const keep[] = {
        L"undo", L"redo", L"cut", L"copy", L"paste", L"selectAll"
    };
    size_t i;
    if (!name) return FALSE;
    for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        if (_wcsicmp(name, keep[i]) == 0) return TRUE;
    return FALSE;
}

/* Trim the default menu to the editing commands (cut, copy, paste, ...),
   dropping Back, Reload, Save as, Print and the like. When nothing is left,
   or the menu cannot be read, no menu is shown. */
static HRESULT STDMETHODCALLTYPE MenuHandler_Invoke(
    ICoreWebView2ContextMenuRequestedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2ContextMenuRequestedEventArgs *args)
{
    ICoreWebView2ContextMenuItemCollection *items = NULL;
    UINT32 count = 0, i = 0;
    BOOL ok = FALSE, last_was_separator = TRUE;
    int commands = 0;
    (void)This; (void)sender;

    if (SUCCEEDED(args->lpVtbl->get_MenuItems(args, &items)) && items &&
        SUCCEEDED(items->lpVtbl->get_Count(items, &count))) {
        ok = TRUE;
        while (i < count) {
            ICoreWebView2ContextMenuItem *item = NULL;
            COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND kind = COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SUBMENU;
            LPWSTR name = NULL;
            BOOL keep = FALSE;

            if (FAILED(items->lpVtbl->GetValueAtIndex(items, i, &item)) || !item) {
                ok = FALSE;
                break;
            }
            item->lpVtbl->get_Kind(item, &kind);
            if (kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR) {
                keep = !last_was_separator;     /* no leading or doubled separators */
            } else if (kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND &&
                       SUCCEEDED(item->lpVtbl->get_Name(item, &name))) {
                keep = is_editing_command(name);
            }
            CoTaskMemFree(name);
            item->lpVtbl->Release(item);

            if (keep) {
                last_was_separator = (kind == COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_SEPARATOR);
                if (!last_was_separator) commands++;
                i++;
            } else if (SUCCEEDED(items->lpVtbl->RemoveValueAtIndex(items, i))) {
                count--;
            } else {
                ok = FALSE;
                break;
            }
        }
        if (ok && count > 0 && last_was_separator)
            items->lpVtbl->RemoveValueAtIndex(items, count - 1);
    }
    if (items) items->lpVtbl->Release(items);
    if (!ok || commands == 0)
        args->lpVtbl->put_Handled(args, TRUE);
    return S_OK;
}

static ICoreWebView2ContextMenuRequestedEventHandlerVtbl g_menuVtbl = {
    MenuHandler_QI, MenuHandler_AddRef, MenuHandler_Release, MenuHandler_Invoke
};

/* ---- COM callback: Process Failed ---- */

typedef struct {
    ICoreWebView2ProcessFailedEventHandlerVtbl *lpVtbl;
    LONG ref;
} FailHandler;

DEFINE_HANDLER_IUNKNOWN(FailHandler, ICoreWebView2ProcessFailedEventHandler)

static HRESULT STDMETHODCALLTYPE FailHandler_Invoke(
    ICoreWebView2ProcessFailedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2ProcessFailedEventArgs *args)
{
    COREWEBVIEW2_PROCESS_FAILED_KIND kind;
    COREWEBVIEW2_PROCESS_FAILED_REASON reason = COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
    ICoreWebView2ProcessFailedEventArgs2 *args2 = NULL;
    int exit_code = 0;
    (void)This; (void)sender;

    if (FAILED(args->lpVtbl->get_ProcessFailedKind(args, &kind)))
        return S_OK;
    if (SUCCEEDED(args->lpVtbl->QueryInterface(args, &IID_ICoreWebView2ProcessFailedEventArgs2,
                                               (void **)&args2)) && args2) {
        args2->lpVtbl->get_Reason(args2, &reason);
        args2->lpVtbl->get_ExitCode(args2, &exit_code);
        args2->lpVtbl->Release(args2);
    }

    switch (kind) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        ui_log(L"WebView2: the %s process ended (reason %d, exit code %d)",
               kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ? L"browser" : L"page",
               (int)reason, exit_code);
        g_ready = FALSE;
        report_failure(kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED
                           ? FAIL_BROWSER_EXITED : FAIL_RENDERER_EXITED, S_OK);
        break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
        /* Raised again every few seconds while the page hangs. Ask while the
           window is on screen, once at a time, and not again soon after the
           user chose to wait. */
        if (g_failure_prompt_open || GetTickCount64() < g_unresponsive_quiet_until ||
            !IsWindowVisible(g_parent_hwnd) || IsIconic(g_parent_hwnd))
            break;
        ui_log(L"WebView2: the page is not responding");
        report_failure(FAIL_UNRESPONSIVE, S_OK);
        break;
    default:
        /* GPU, utility and iframe processes are restarted by WebView2. */
        ui_log(L"WebView2: a helper process ended (kind %d, reason %d, exit code %d)",
               (int)kind, (int)reason, exit_code);
        break;
    }
    return S_OK;
}

static ICoreWebView2ProcessFailedEventHandlerVtbl g_failVtbl = {
    FailHandler_QI, FailHandler_AddRef, FailHandler_Release, FailHandler_Invoke
};

/* ---- The app page ---- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Full path of the UI page: web\index.html beside the exe. Debug builds
   also look in ..\web so the exe can run from the build folder. */
static BOOL find_page_path(wchar_t *out, DWORD cap)
{
    wchar_t exe_dir[MAX_PATH], path[MAX_PATH];
    wchar_t *slash;
    DWORD n;

    n = GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';
    if (wcslen(exe_dir) + 24 >= MAX_PATH) return FALSE;
    swprintf_s(path, MAX_PATH, L"%s\\web\\index.html", exe_dir);
#ifdef _DEBUG
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        wchar_t dev_path[MAX_PATH];
        swprintf_s(dev_path, MAX_PATH, L"%s\\..\\web\\index.html", exe_dir);
        if (GetFileAttributesW(dev_path) != INVALID_FILE_ATTRIBUTES)
            wcscpy_s(path, MAX_PATH, dev_path);
    }
#endif
    n = GetFullPathNameW(path, cap, out, NULL);
    return n > 0 && n < cap;
}

/* file: URL of a full path: "C:\a b\x.html" -> "file:///C:/a%20b/x.html",
   "\\server\share\x" -> "file://server/share/x". Bytes outside the URL
   path characters (space, '#', '%', '?', non-ASCII, ...) are
   percent-encoded as UTF-8. */
static BOOL path_to_file_url(const wchar_t *path, wchar_t *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    char utf8[MAX_PATH * 4];  /* at most 3 bytes per UTF-16 unit */
    const char *s;
    size_t o;

    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, (int)sizeof(utf8), NULL, NULL) <= 0)
        return FALSE;
    if (cap < 16) return FALSE;
    wcscpy_s(out, cap, (utf8[0] == '\\' && utf8[1] == '\\') ? L"file:" : L"file:///");
    o = wcslen(out);
    for (s = utf8; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (o + 4 >= cap) return FALSE;
        if (c == '\\') {
            out[o++] = L'/';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || strchr("-._~!$&'()*+,;=:@", c)) {
            out[o++] = (wchar_t)c;
        } else {
            out[o++] = L'%';
            out[o++] = (wchar_t)hex[c >> 4];
            out[o++] = (wchar_t)hex[c & 15];
        }
    }
    out[o] = L'\0';
    return TRUE;
}

/* Full Windows path of a file: URI, the reverse of path_to_file_url:
   percent escapes are decoded as UTF-8 and the query and fragment are
   dropped. Returns FALSE for anything that is not a local or UNC file. */
static BOOL file_uri_to_path(const wchar_t *uri, wchar_t *out, DWORD cap)
{
    char utf8[MAX_PATH * 12];
    wchar_t wide[MAX_PATH + 2];
    const char *src;
    size_t n;
    int len, i, o = 0, prefix = 0;
    DWORD full;

    if (_wcsnicmp(uri, L"file://", 7) != 0) return FALSE;
    uri += 7;
    n = wcscspn(uri, L"?#");
    if (n == 0 || n >= MAX_PATH * 9) return FALSE;
    len = WideCharToMultiByte(CP_UTF8, 0, uri, (int)n, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (len <= 0) return FALSE;

    /* Decode in place: the output never runs ahead of the input. */
    for (i = 0; i < len; i++) {
        int c = (unsigned char)utf8[i];
        if (c == '%' && i + 2 < len &&
            hex_value(utf8[i + 1]) >= 0 && hex_value(utf8[i + 2]) >= 0) {
            c = hex_value(utf8[i + 1]) * 16 + hex_value(utf8[i + 2]);
            i += 2;
        }
        if (c == 0) return FALSE;
        utf8[o++] = (char)(c == '/' ? '\\' : c);
    }
    utf8[o] = '\0';

    if (utf8[0] == '\\') {
        /* "file:///C:/..." is a drive path. */
        src = utf8 + 1;
        if (!((src[0] >= 'a' && src[0] <= 'z') || (src[0] >= 'A' && src[0] <= 'Z')) ||
            src[1] != ':')
            return FALSE;
    } else {
        /* "file://server/share/..." is a UNC path. */
        src = utf8;
        wide[0] = wide[1] = L'\\';
        prefix = 2;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1,
                            wide + prefix, MAX_PATH) <= 0)
        return FALSE;
    full = GetFullPathNameW(wide, cap, out, NULL);
    return full > 0 && full < cap;
}

/* TRUE when uri is the app page (any query or fragment aside). */
static BOOL is_app_page_uri(const wchar_t *uri)
{
    wchar_t path[MAX_PATH];

    if (!uri || !g_page_path[0]) return FALSE;
    if (g_page_uri) {
        size_t n = wcscspn(g_page_uri, L"?#");
        if (wcsncmp(uri, g_page_uri, n) == 0 &&
            (uri[n] == L'\0' || uri[n] == L'?' || uri[n] == L'#'))
            return TRUE;
    }
    return file_uri_to_path(uri, path, MAX_PATH) &&
           CompareStringOrdinal(path, -1, g_page_path, -1, TRUE) == CSTR_EQUAL;
}

/* ---- Callback implementations ---- */

/* Start creating a controller in env for the current generation; the page
   follows in CtrlH_Invoke. Returns the synchronous result. */
static HRESULT start_controller(ICoreWebView2Environment *env)
{
    CtrlHandler *ch;
    HRESULT hr;

    ch = (CtrlHandler *)calloc(1, sizeof(CtrlHandler));
    if (!ch) return E_OUTOFMEMORY;
    ch->lpVtbl = &g_ctrlVtbl;
    ch->ref = 1;
    ch->generation = g_generation;

    hr = env->lpVtbl->CreateCoreWebView2Controller(env, g_parent_hwnd,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)ch);
    if (FAILED(hr)) {
        ui_log(L"WebView2: Controller creation failed (0x%08X)", (unsigned)hr);
        ch->lpVtbl->Release((ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)ch);
    }
    return hr;
}

/* Release the kept environment, if any. */
static void release_env(void)
{
    if (g_env) {
        g_env->lpVtbl->Release(g_env);
        g_env = NULL;
    }
}

static HRESULT STDMETHODCALLTYPE EnvH_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env)
{
    HRESULT hr;

    /* A newer start, or shutdown, replaced this one. */
    if (((EnvHandler *)This)->generation != g_generation)
        return S_OK;

    if (FAILED(errorCode) || !env) {
        ui_log(L"WebView2: Environment creation failed (0x%08X)", (unsigned)errorCode);
        start_failed(FAILED(errorCode) ? errorCode : E_FAIL);
        return S_OK;
    }

    release_env();
    g_env = env;
    env->lpVtbl->AddRef(env);

    hr = start_controller(env);
    if (FAILED(hr))
        start_failed(hr);
    return S_OK;
}

/* Turn off the browser features the app does not use. Settings that need a
   newer runtime are skipped when it does not have them. */
static void configure_webview(ICoreWebView2 *webview, ICoreWebView2Controller *controller,
                              BOOL menu_filtered)
{
    ICoreWebView2Settings *settings = NULL;
    ICoreWebView2Settings3 *settings3 = NULL;
    ICoreWebView2Settings4 *settings4 = NULL;
    ICoreWebView2Settings6 *settings6 = NULL;
    ICoreWebView2Controller4 *controller4 = NULL;

    if (SUCCEEDED(webview->lpVtbl->get_Settings(webview, &settings)) && settings) {
        settings->lpVtbl->put_AreDevToolsEnabled(settings, FALSE);
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_AreHostObjectsAllowed(settings, FALSE);
        /* The default menu stays only when ContextMenuRequested trims it. */
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, menu_filtered);

        if (SUCCEEDED(settings->lpVtbl->QueryInterface(settings, &IID_ICoreWebView2Settings3,
                                                       (void **)&settings3)) && settings3) {
            /* F5/Ctrl+R reload, Ctrl+P print, Ctrl+F find, Alt+Left back, ...
               Editing keys stay, and AcceleratorKeyPressed still sees every
               key, so F11 and the zoom keys are handled there. */
            settings3->lpVtbl->put_AreBrowserAcceleratorKeysEnabled(settings3, FALSE);
            settings3->lpVtbl->Release(settings3);
        }
        if (SUCCEEDED(settings->lpVtbl->QueryInterface(settings, &IID_ICoreWebView2Settings4,
                                                       (void **)&settings4)) && settings4) {
            settings4->lpVtbl->put_IsPasswordAutosaveEnabled(settings4, FALSE);
            settings4->lpVtbl->put_IsGeneralAutofillEnabled(settings4, FALSE);
            settings4->lpVtbl->Release(settings4);
        }
        if (SUCCEEDED(settings->lpVtbl->QueryInterface(settings, &IID_ICoreWebView2Settings6,
                                                       (void **)&settings6)) && settings6) {
            settings6->lpVtbl->put_IsSwipeNavigationEnabled(settings6, FALSE);
            settings6->lpVtbl->Release(settings6);
        }
        settings->lpVtbl->Release(settings);
    }

    if (SUCCEEDED(controller->lpVtbl->QueryInterface(controller, &IID_ICoreWebView2Controller4,
                                                     (void **)&controller4)) && controller4) {
        /* Files or links dropped on the window would otherwise navigate to them. */
        controller4->lpVtbl->put_AllowExternalDrop(controller4, FALSE);
        controller4->lpVtbl->Release(controller4);
    }
}

/* Register the event handlers. Returns TRUE when the context menu is
   filtered by MenuHandler (runtimes without ICoreWebView2_11 get no
   default menu at all). */
static BOOL register_handlers(ICoreWebView2 *webview, ICoreWebView2Controller *controller)
{
    EventRegistrationToken token;
    MsgHandler *mh;
    AcceleratorHandler *ah;
    NavHandler *nh;
    NewWindowHandler *wh;
    FailHandler *fh;
    ICoreWebView2_4 *webview4 = NULL;
    ICoreWebView2_11 *webview11 = NULL;
    BOOL menu_filtered = FALSE;
    int frame;

    mh = (MsgHandler *)calloc(1, sizeof(MsgHandler));
    if (mh) {
        mh->lpVtbl = &g_msgVtbl;
        mh->ref = 1;
        webview->lpVtbl->add_WebMessageReceived(webview,
            (ICoreWebView2WebMessageReceivedEventHandler *)mh, &token);
        mh->lpVtbl->Release((ICoreWebView2WebMessageReceivedEventHandler *)mh);
    }

    ah = (AcceleratorHandler *)calloc(1, sizeof(AcceleratorHandler));
    if (ah) {
        ah->lpVtbl = &g_acceleratorVtbl;
        ah->ref = 1;
        controller->lpVtbl->add_AcceleratorKeyPressed(controller,
            (ICoreWebView2AcceleratorKeyPressedEventHandler *)ah, &token);
        ah->lpVtbl->Release((ICoreWebView2AcceleratorKeyPressedEventHandler *)ah);
    }

    for (frame = 0; frame <= 1; frame++) {
        nh = (NavHandler *)calloc(1, sizeof(NavHandler));
        if (!nh) continue;
        nh->lpVtbl = &g_navVtbl;
        nh->ref = 1;
        nh->frame = frame;
        if (frame)
            webview->lpVtbl->add_FrameNavigationStarting(webview,
                (ICoreWebView2NavigationStartingEventHandler *)nh, &token);
        else
            webview->lpVtbl->add_NavigationStarting(webview,
                (ICoreWebView2NavigationStartingEventHandler *)nh, &token);
        nh->lpVtbl->Release((ICoreWebView2NavigationStartingEventHandler *)nh);
    }

    wh = (NewWindowHandler *)calloc(1, sizeof(NewWindowHandler));
    if (wh) {
        wh->lpVtbl = &g_newWindowVtbl;
        wh->ref = 1;
        webview->lpVtbl->add_NewWindowRequested(webview,
            (ICoreWebView2NewWindowRequestedEventHandler *)wh, &token);
        wh->lpVtbl->Release((ICoreWebView2NewWindowRequestedEventHandler *)wh);
    }

    fh = (FailHandler *)calloc(1, sizeof(FailHandler));
    if (fh) {
        fh->lpVtbl = &g_failVtbl;
        fh->ref = 1;
        webview->lpVtbl->add_ProcessFailed(webview,
            (ICoreWebView2ProcessFailedEventHandler *)fh, &token);
        fh->lpVtbl->Release((ICoreWebView2ProcessFailedEventHandler *)fh);
    }

    if (SUCCEEDED(webview->lpVtbl->QueryInterface(webview, &IID_ICoreWebView2_4,
                                                  (void **)&webview4)) && webview4) {
        DownloadHandler *dh = (DownloadHandler *)calloc(1, sizeof(DownloadHandler));
        if (dh) {
            dh->lpVtbl = &g_downloadVtbl;
            dh->ref = 1;
            webview4->lpVtbl->add_DownloadStarting(webview4,
                (ICoreWebView2DownloadStartingEventHandler *)dh, &token);
            dh->lpVtbl->Release((ICoreWebView2DownloadStartingEventHandler *)dh);
        }
        webview4->lpVtbl->Release(webview4);
    }

    if (SUCCEEDED(webview->lpVtbl->QueryInterface(webview, &IID_ICoreWebView2_11,
                                                  (void **)&webview11)) && webview11) {
        MenuHandler *uh = (MenuHandler *)calloc(1, sizeof(MenuHandler));
        if (uh) {
            uh->lpVtbl = &g_menuVtbl;
            uh->ref = 1;
            menu_filtered = SUCCEEDED(webview11->lpVtbl->add_ContextMenuRequested(webview11,
                (ICoreWebView2ContextMenuRequestedEventHandler *)uh, &token));
            uh->lpVtbl->Release((ICoreWebView2ContextMenuRequestedEventHandler *)uh);
        }
        webview11->lpVtbl->Release(webview11);
    }
    return menu_filtered;
}

/* Navigate webview to the app page (a new WebView, or one whose page
   process ended). */
static HRESULT navigate_to_page(ICoreWebView2 *webview)
{
    wchar_t url[MAX_PATH * 9 + 16];     /* up to 3 escapes per path character */
    HRESULT hr;

    if (!g_page_path[0] && !find_page_path(g_page_path, MAX_PATH)) {
        ui_log(L"WebView2: cannot locate web\\index.html");
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    if (GetFileAttributesW(g_page_path) == INVALID_FILE_ATTRIBUTES)
        ui_log(L"WebView2: %s is missing", g_page_path);
    if (!path_to_file_url(g_page_path, url, sizeof(url) / sizeof(url[0]))) {
        ui_log(L"WebView2: cannot build a URL for %s", g_page_path);
        return E_INVALIDARG;
    }

    g_expect_own_navigation = TRUE;
    hr = webview->lpVtbl->Navigate(webview, url);
    if (FAILED(hr)) {
        ui_log(L"WebView2: Navigate failed (0x%08X)", (unsigned)hr);
        g_expect_own_navigation = FALSE;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE CtrlH_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller)
{
    ICoreWebView2 *webview = NULL;
    HRESULT hr;

    if (((CtrlHandler *)This)->generation != g_generation) {
        /* A newer start, or shutdown, replaced this one. */
        if (SUCCEEDED(errorCode) && controller)
            controller->lpVtbl->Close(controller);
        return S_OK;
    }

    if (FAILED(errorCode) || !controller) {
        ui_log(L"WebView2: Controller creation failed (0x%08X)", (unsigned)errorCode);
        start_failed(FAILED(errorCode) ? errorCode : E_FAIL);
        return S_OK;
    }

    controller->lpVtbl->get_CoreWebView2(controller, &webview);
    if (!webview) {
        ui_log(L"WebView2: Failed to get CoreWebView2");
        controller->lpVtbl->Close(controller);
        start_failed(E_NOINTERFACE);
        return S_OK;
    }

    /* The WebView started: a later failure is a new one. */
    g_restart_retries = 0;
    g_busy_retries = 0;
    g_controller = controller;
    controller->lpVtbl->AddRef(controller);
    g_webview = webview;

    /* Lock the WebView down before it loads anything. */
    configure_webview(webview, controller, register_handlers(webview, controller));

    /* Resize to fill parent */
    webview2_resize(g_parent_hwnd);

    hr = navigate_to_page(webview);
    if (FAILED(hr)) {
        report_failure(FAIL_START, hr);
        return S_OK;
    }

    ui_log(L"WebView2: Ready");
    return S_OK;
}

/* ---- Start, restart and failure prompt ---- */

/* Close and release the current WebView, if any. */
static void release_webview(void)
{
    g_ready = FALSE;
    g_expect_own_navigation = FALSE;
    if (g_controller) {
        g_controller->lpVtbl->Close(g_controller);
        g_controller->lpVtbl->Release(g_controller);
        g_controller = NULL;
    }
    if (g_webview) {
        g_webview->lpVtbl->Release(g_webview);
        g_webview = NULL;
    }
}

/* Start creating the environment; the controller and the page follow in
   EnvH_Invoke and CtrlH_Invoke. Returns the synchronous result. */
static HRESULT start_environment(void)
{
    EnvHandler *eh;
    wchar_t udf[MAX_PATH];
    wchar_t base[MAX_PATH];
    DWORD n;
    HRESULT hr;

    /* User data folder */
    n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    if (wcslen(base) + 32 >= MAX_PATH)
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    swprintf_s(udf, MAX_PATH, g_alt_user_data ? L"%s\\AppSandbox\\WebView2Data.2"
                                              : L"%s\\AppSandbox\\WebView2Data", base);

    eh = (EnvHandler *)calloc(1, sizeof(EnvHandler));
    if (!eh) return E_OUTOFMEMORY;
    eh->lpVtbl = &g_envVtbl;
    eh->ref = 1;
    eh->generation = ++g_generation;

    hr = pfnCreateEnv(NULL, udf, NULL,
        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)eh);
    if (FAILED(hr))
        eh->lpVtbl->Release((ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)eh);
    return hr;
}

static void kill_restart_timer(void)
{
    if (g_restart_timer) {
        KillTimer(NULL, g_restart_timer);
        g_restart_timer = 0;
    }
}

/* Replace the WebView with a new environment, controller and page. */
static void start_fresh(void)
{
    HRESULT hr;

    release_webview();
    release_env();
    hr = start_environment();
    if (FAILED(hr)) {
        ui_log(L"WebView2: CreateEnvironment failed (0x%08X)", (unsigned)hr);
        start_failed(hr);
    }
}

static void CALLBACK retry_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)time;
    KillTimer(NULL, id);
    if (id == g_restart_timer) g_restart_timer = 0;
    if (g_shutting_down || !IsWindow(g_parent_hwnd))
        return;
    ui_log(L"WebView2: starting again");
    start_fresh();
}

/* The environment or controller did not start. The restart the user asked
   for is retried once with a new environment; otherwise ask the user. */
static void start_failed(HRESULT hr)
{
    if (g_shutting_down) return;
    if (hr == WV2_E_BUSY && (g_busy_retries < BUSY_RETRIES || !g_alt_user_data)) {
        UINT delay = BUSY_RETRY_MS;
        if (g_busy_retries < BUSY_RETRIES) {
            if (g_busy_retries == 0)
                ui_log(L"WebView2: its data folder is still in use by another WebView2 process; "
                       L"waiting for it to close");
            g_busy_retries++;
        } else {
            g_alt_user_data = TRUE;
            delay = 10;
            ui_log(L"WebView2: the data folder stayed in use; using WebView2Data.2 instead");
        }
        kill_restart_timer();
        g_restart_timer = SetTimer(NULL, 0, delay, retry_timer_proc);
        if (g_restart_timer) return;
    }
    if (g_restart_retries > 0) {
        g_restart_retries--;
        kill_restart_timer();
        g_restart_timer = SetTimer(NULL, 0, RESTART_RETRY_MS, retry_timer_proc);
        if (g_restart_timer) {
            ui_log(L"WebView2: start failed (0x%08X), trying again", (unsigned)hr);
            return;
        }
    }
    g_restart_retries = 0;
    report_failure(FAIL_START, hr);
}

/* Reload the window after a failure of the given kind. The page asks the
   host for its state again with "uiReady". */
static void restart_webview(int kind)
{
    HRESULT hr;

    g_declined_failure = FAIL_NONE;
    g_restart_retries = 0;
    g_busy_retries = 0;
    kill_restart_timer();

    /* The page process ended but the browser process runs: load the page
       again in the same WebView, which starts a new page process. */
    if (kind == FAIL_RENDERER_EXITED && g_webview && g_controller) {
        ui_log(L"WebView2: reloading the page");
        g_ready = FALSE;
        if (SUCCEEDED(navigate_to_page(g_webview)))
            return;
    }

    ui_log(L"WebView2: reloading the window");
    release_webview();      /* also ends a hung page process */
    g_restart_retries = 1;

    /* The page hangs but the browser process runs: make the new WebView in
       the same environment. If that fails, start_failed retries with a new
       environment. */
    if (kind == FAIL_UNRESPONSIVE && g_env) {
        ++g_generation;     /* ignore any start still in flight */
        hr = start_controller(g_env);
        if (FAILED(hr))
            start_failed(hr);
        return;
    }

    /* The browser process ended, or the WebView never started. */
    start_fresh();
}

/* F5 or Ctrl+R after a declined failure. Runs from the message loop, not
   from inside the WebView2 key event, since the restart closes the
   controller that raised it. */
static void CALLBACK key_reload_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)time;
    KillTimer(NULL, id);
    if (id == g_restart_timer) g_restart_timer = 0;
    if (g_shutting_down || !IsWindow(g_parent_hwnd) || g_failure_prompt_open ||
        g_declined_failure == FAIL_NONE)
        return;
    restart_webview(g_declined_failure);
}

static void schedule_key_reload(void)
{
    if (g_restart_timer) return;    /* a restart or retry is already pending */
    g_restart_timer = SetTimer(NULL, 0, 0, key_reload_timer_proc);
}

/* Ask again about a declined failure (the window was shown or restored). */
static void ask_again_if_declined(void)
{
    if (g_declined_failure != FAIL_NONE && !g_failure_prompt_open && !g_restart_timer)
        report_failure(g_declined_failure, g_declined_hr);
}

/* Subclass of the parent window. While a failure is declined the window
   shows no working page: ask again when it comes back from the tray or
   from minimized, and let F5 and Ctrl+R reload it when the parent has the
   keyboard focus (there is no WebView, or it does not take keys). */
static LRESULT CALLBACK parent_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR id, DWORD_PTR data)
{
    static BOOL minimized;
    (void)data;

    switch (msg) {
    case WM_SHOWWINDOW:
        if (wp && lp == 0)
            ask_again_if_declined();
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            minimized = TRUE;
        } else if (minimized && (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED)) {
            minimized = FALSE;
            ask_again_if_declined();
        }
        break;
    case WM_KEYDOWN:
        if (g_declined_failure != FAIL_NONE && !(lp & (1 << 30)) &&
            (wp == VK_F5 || (wp == 'R' && GetKeyState(VK_CONTROL) < 0))) {
            schedule_key_reload();
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, parent_subclass_proc, id);
        g_parent_subclassed = FALSE;
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* Ask whether to reload the window. Returns TRUE for Reload. */
static BOOL ask_reload(int kind, HRESULT hr)
{
    TASKDIALOGCONFIG tdc;
    TASKDIALOG_BUTTON buttons[2];
    const wchar_t *title;
    const wchar_t *dismiss = L"Not now";
    wchar_t content[600];
    wchar_t text[800];
    /* Without a visible owner (window in the tray or minimized) the dialog
       gets its own taskbar button. */
    HWND owner = (IsWindowVisible(g_parent_hwnd) && !IsIconic(g_parent_hwnd))
                 ? g_parent_hwnd : NULL;
    int button = 0;

    switch (kind) {
    case FAIL_UNRESPONSIVE:
        title = L"The App Sandbox window is not responding";
        wcscpy_s(content, sizeof(content) / sizeof(content[0]),
                 L"Running virtual machines are not affected. You can wait for the "
                 L"window to respond, or reload it.");
        dismiss = L"Wait";
        break;
    case FAIL_START:
        title = L"The App Sandbox window could not be shown";
        swprintf_s(content, sizeof(content) / sizeof(content[0]),
                   L"Microsoft Edge WebView2, which draws this window, did not start "
                   L"(error 0x%08X). Running virtual machines are not affected.\n\n"
                   L"If this keeps happening, repair the Microsoft Edge WebView2 Runtime "
                   L"in Settings > Apps > Installed apps.", (unsigned)hr);
        break;
    default:
        title = L"The App Sandbox window stopped working";
        wcscpy_s(content, sizeof(content) / sizeof(content[0]),
                 L"A Microsoft Edge WebView2 process that draws this window ended "
                 L"unexpectedly. Running virtual machines are not affected.\n\n"
                 L"Reload the window to continue.");
        break;
    }

    buttons[0].nButtonID = ID_RELOAD;
    buttons[0].pszButtonText = L"Reload";
    buttons[1].nButtonID = IDCANCEL;
    buttons[1].pszButtonText = dismiss;

    ZeroMemory(&tdc, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = owner;
    tdc.hInstance = g_hInstance;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = L"App Sandbox";
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszMainInstruction = title;
    tdc.pszContent = content;
    tdc.cButtons = 2;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = ID_RELOAD;

    if (SUCCEEDED(TaskDialogIndirect(&tdc, &button, NULL, NULL)))
        return button == ID_RELOAD;

    swprintf_s(text, sizeof(text) / sizeof(text[0]), L"%s.\n\n%s\n\nReload the window now?",
               title, content);
    return MessageBoxW(owner, text, L"App Sandbox", MB_ICONWARNING | MB_YESNO) == IDYES;
}

/* Runs from the message loop (a zero-delay thread timer), not from inside
   the WebView2 callback that reported the failure, since a modal dialog
   there would run a nested message loop inside WebView2's event. */
static void CALLBACK failure_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    int kind;
    HRESULT hr;
    BOOL reload;
    (void)hwnd; (void)msg; (void)time;

    KillTimer(NULL, id);
    g_failure_timer = 0;
    kind = g_pending_failure;
    hr = g_pending_hr;
    g_pending_failure = FAIL_NONE;
    if (kind == FAIL_NONE || g_shutting_down || !IsWindow(g_parent_hwnd))
        return;

    g_failure_prompt_open = TRUE;
    reload = ask_reload(kind, hr);
    g_failure_prompt_open = FALSE;

    /* The window may have closed while the dialog was open. */
    if (g_shutting_down || !IsWindow(g_parent_hwnd))
        return;

    if (reload) {
        g_pending_failure = FAIL_NONE;      /* the restart covers it */
        restart_webview(kind);
        return;
    }
    if (kind == FAIL_UNRESPONSIVE) {
        g_unresponsive_quiet_until = GetTickCount64() + UNRESPONSIVE_QUIET_MS;
    } else {
        /* Remembered so the user can still reload: see g_declined_failure. */
        if (kind >= g_declined_failure) {
            g_declined_failure = kind;
            g_declined_hr = hr;
        }
        if (kind == FAIL_BROWSER_EXITED) {
            /* The WebView and its environment are dead: release them, so
               the parent window gets the keyboard focus for F5 and Ctrl+R. */
            release_webview();
            release_env();
            if (GetForegroundWindow() == g_parent_hwnd)
                SetFocus(g_parent_hwnd);
        }
    }
    ui_log(L"WebView2: reload declined");
    /* Something worse happened while the dialog was open: ask about it. */
    if (g_pending_failure > kind)
        report_failure(g_pending_failure, g_pending_hr);
    else
        g_pending_failure = FAIL_NONE;
}

/* Record a failure and ask the user about it from the message loop. A
   failure that arrives while the dialog is open is kept for afterwards. */
static void report_failure(int kind, HRESULT hr)
{
    if (g_shutting_down) return;
    if (kind >= g_pending_failure) {
        g_pending_failure = kind;
        g_pending_hr = hr;
    }
    if (!g_failure_timer && !g_failure_prompt_open) {
        g_failure_timer = SetTimer(NULL, 0, 0, failure_timer_proc);
        if (!g_failure_timer)
            ui_log(L"WebView2: cannot show the reload prompt (error %lu)", GetLastError());
    }
}

/* ---- Public API ---- */

static BOOL load_webview2_loader(void)
{
    wchar_t dll_path[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    /* Next to the exe (where builds and packages put it) */
    swprintf_s(dll_path, MAX_PATH, L"%s\\WebView2Loader.dll", exe_dir);
    g_wv2_module = LoadLibraryW(dll_path);

#ifdef _DEBUG
    /* Development: the vendor directory of the source tree */
    if (!g_wv2_module) {
#ifdef _M_ARM64
        swprintf_s(dll_path, MAX_PATH, L"%s\\..\\vendor\\webview2\\arm64\\WebView2Loader.dll", exe_dir);
#else
        swprintf_s(dll_path, MAX_PATH, L"%s\\..\\vendor\\webview2\\x64\\WebView2Loader.dll", exe_dir);
#endif
        g_wv2_module = LoadLibraryW(dll_path);
    }
#endif

    if (!g_wv2_module) {
        ui_log(L"WebView2: Failed to load WebView2Loader.dll");
        return FALSE;
    }

    pfnCreateEnv = (PFN_CreateCoreWebView2EnvironmentWithOptions)
        GetProcAddress(g_wv2_module, "CreateCoreWebView2EnvironmentWithOptions");
    pfnGetVersion = (PFN_GetAvailableCoreWebView2BrowserVersionString)
        GetProcAddress(g_wv2_module, "GetAvailableCoreWebView2BrowserVersionString");

    if (!pfnCreateEnv) {
        ui_log(L"WebView2: Missing CreateCoreWebView2EnvironmentWithOptions");
        FreeLibrary(g_wv2_module);
        g_wv2_module = NULL;
        return FALSE;
    }

    return TRUE;
}

BOOL webview2_init(HWND parent, HINSTANCE hInstance)
{
    HRESULT hr;
    LPWSTR version = NULL;

    g_parent_hwnd = parent;
    g_hInstance = hInstance;
    g_ready = FALSE;
    g_queue_count = 0;
    g_shutting_down = FALSE;

    if (!load_webview2_loader())
        return FALSE;

    /* Check runtime availability */
    if (pfnGetVersion) {
        hr = pfnGetVersion(NULL, &version);
        if (SUCCEEDED(hr) && version) {
            ui_log(L"WebView2 Runtime: %s", version);
            CoTaskMemFree(version);
        } else {
            ui_log(L"WebView2 Runtime not found. Please install Microsoft Edge WebView2 Runtime.");
            return FALSE;
        }
    }

    g_parent_subclassed = SetWindowSubclass(parent, parent_subclass_proc,
                                            PARENT_SUBCLASS_ID, 0);

    hr = start_environment();
    if (FAILED(hr)) {
        /* The runtime is there, so offer to try again rather than leave the
           window empty. */
        ui_log(L"WebView2: CreateEnvironment failed (0x%08X)", (unsigned)hr);
        if (hr == WV2_E_BUSY) start_failed(hr);   /* waits, then retries */
        else report_failure(FAIL_START, hr);
    }

    return TRUE;
}

void webview2_post(const wchar_t *json)
{
    if (!json) return;

    if (g_ready && g_webview) {
        g_webview->lpVtbl->PostWebMessageAsJson(g_webview, json);
    } else {
        /* Queue for later */
        if (g_queue_count < MAX_QUEUED) {
            size_t len = wcslen(json) + 1;
            wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
            if (copy) {
                wcscpy_s(copy, len, json);
                g_queue[g_queue_count++] = copy;
            }
        }
    }
}

void webview2_flush_queue(void)
{
    int i;
    if (!g_ready || !g_webview) return;
    for (i = 0; i < g_queue_count; i++) {
        g_webview->lpVtbl->PostWebMessageAsJson(g_webview, g_queue[i]);
        free(g_queue[i]);
    }
    g_queue_count = 0;
}

void webview2_resize(HWND parent)
{
    if (g_controller) {
        RECT rc;
        GetClientRect(parent, &rc);
        g_controller->lpVtbl->put_Bounds(g_controller, rc);
    }
}

void webview2_cleanup(void)
{
    int i;

    g_shutting_down = TRUE;
    ++g_generation;             /* ignore any start still in flight */
    if (g_failure_timer) {
        KillTimer(NULL, g_failure_timer);
        g_failure_timer = 0;
    }
    kill_restart_timer();
    g_pending_failure = FAIL_NONE;
    g_declined_failure = FAIL_NONE;
    g_restart_retries = 0;

    if (g_parent_subclassed) {
        if (IsWindow(g_parent_hwnd))
            RemoveWindowSubclass(g_parent_hwnd, parent_subclass_proc, PARENT_SUBCLASS_ID);
        g_parent_subclassed = FALSE;
    }

    release_webview();
    release_env();
    for (i = 0; i < g_queue_count; i++)
        free(g_queue[i]);
    g_queue_count = 0;
    g_ready = FALSE;

    if (g_page_uri) {
        CoTaskMemFree(g_page_uri);
        g_page_uri = NULL;
    }

    if (g_wv2_module) {
        FreeLibrary(g_wv2_module);
        g_wv2_module = NULL;
    }
}

BOOL webview2_is_ready(void)
{
    return g_ready;
}

/* ---- JSON builder ---- */

void jb_init(JsonBuilder *jb, wchar_t *buf, size_t cap)
{
    jb->buf = buf;
    jb->cap = cap;
    jb->len = 0;
    jb->count = 0;
    if (cap > 0) buf[0] = L'\0';
}

void jb_append(JsonBuilder *jb, const wchar_t *s)
{
    size_t slen = wcslen(s);
    if (jb->len + slen < jb->cap) {
        wcscpy_s(jb->buf + jb->len, jb->cap - jb->len, s);
        jb->len += slen;
    }
}

void jb_append_escaped(JsonBuilder *jb, const wchar_t *s)
{
    const wchar_t *p;
    for (p = s; *p; p++) {
        if (jb->len + 6 >= jb->cap) break;
        switch (*p) {
        case L'\\': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'\\'; break;
        case L'"':  jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'"'; break;
        case L'\n': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'n'; break;
        case L'\r': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'r'; break;
        case L'\t': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L't'; break;
        default:    jb->buf[jb->len++] = *p; break;
        }
    }
    jb->buf[jb->len] = L'\0';
}

void jb_object_begin(JsonBuilder *jb)
{
    jb_append(jb, L"{");
    jb->count = 0;
}

void jb_object_end(JsonBuilder *jb)
{
    jb_append(jb, L"}");
}

void jb_array_begin(JsonBuilder *jb, const wchar_t *key)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":[");
    jb->count = 0;
}

void jb_array_end(JsonBuilder *jb)
{
    jb_append(jb, L"]");
    jb->count = 1; /* so next field gets a comma */
}

void jb_string(JsonBuilder *jb, const wchar_t *key, const wchar_t *val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":\"");
    jb_append_escaped(jb, val ? val : L"");
    jb_append(jb, L"\"");
    jb->count++;
}

void jb_int(JsonBuilder *jb, const wchar_t *key, int val)
{
    wchar_t tmp[32];
    if (jb->count > 0) jb_append(jb, L",");
    swprintf_s(tmp, 32, L"\"%s\":%d", key, val);
    jb_append(jb, tmp);
    jb->count++;
}

void jb_bool(JsonBuilder *jb, const wchar_t *key, BOOL val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, val ? L"\":true" : L"\":false");
    jb->count++;
}

/* ---- Simple JSON parser ---- */

/* Skip quoted values so they cannot be mistaken for property names. */
static const wchar_t *json_find_value(const wchar_t *json, const wchar_t *key)
{
    const wchar_t *p = json;
    size_t key_len;
    if (!json || !key) return NULL;
    key_len = wcslen(key);
    while (*p) {
        const wchar_t *start, *end;
        if (*p != L'"') { p++; continue; }
        start = ++p;
        while (*p && *p != L'"') {
            if (*p == L'\\' && p[1]) p += 2;
            else p++;
        }
        if (!*p) return NULL;
        end = p++;
        while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') p++;
        if (*p != L':' || (size_t)(end - start) != key_len ||
            wcsncmp(start, key, key_len) != 0) continue;
        p++;
        while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') p++;
        return p;
    }
    return NULL;
}

BOOL json_has_key(const wchar_t *json, const wchar_t *key)
{
    return json_find_value(json, key) != NULL;
}

BOOL json_get_string(const wchar_t *json, const wchar_t *key,
                     wchar_t *out, size_t out_len)
{
    const wchar_t *p = json_find_value(json, key);
    size_t o = 0;

    if (!p || !out || !out_len || *p != L'"') return FALSE;
    p++;

    /* Copy the value into `out`, DECODING JSON string escapes. \uXXXX -- what
       json.dumps/ensure_ascii and many HTTP clients emit for non-ASCII -- and \" \\ are
       decoded rather than stored as literal text, so e.g. non-ASCII passwords on the
       headless path stay intact. A \uXXXX yields one UTF-16 code unit directly; an
       astral character arrives as two \u escapes (a surrogate pair) and both units are
       stored, which is already valid UTF-16. */
    while (*p && *p != L'"') {
        wchar_t ch;
        if (*p == L'\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case L'"':  ch = L'"';  p++; break;
                case L'\\': ch = L'\\'; p++; break;
                case L'/':  ch = L'/';  p++; break;
                case L'b':  ch = L'\b'; p++; break;
                case L'f':  ch = L'\f'; p++; break;
                case L'n':  ch = L'\n'; p++; break;
                case L'r':  ch = L'\r'; p++; break;
                case L't':  ch = L'\t'; p++; break;
                case L'u': {
                    unsigned v = 0; int i;
                    p++;
                    for (i = 0; i < 4; i++) {
                        wchar_t h = *p;
                        if (h >= L'0' && h <= L'9') v = (v << 4) | (unsigned)(h - L'0');
                        else if (h >= L'a' && h <= L'f') v = (v << 4) | (unsigned)(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') v = (v << 4) | (unsigned)(h - L'A' + 10);
                        else { out[0] = 0; return FALSE; }
                        p++;
                    }
                    ch = (wchar_t)v;
                    break;
                }
                default: out[0] = 0; return FALSE;
            }
        } else {
            ch = *p;
            p++;
            if (ch < 0x20) { out[0] = 0; return FALSE; }
        }
        /* Embedded NUL would silently turn a decoded path into its prefix. */
        if (ch == 0) { out[0] = 0; return FALSE; }
        if (o + 1 >= out_len) { out[o] = 0; return FALSE; }
        out[o++] = ch;
    }
    if (*p != L'"') { out[0] = 0; return FALSE; }
    out[o] = 0;
    return TRUE;
}

BOOL json_get_int(const wchar_t *json, const wchar_t *key, int *out)
{
    const wchar_t *p = json_find_value(json, key);
    if (!p || !out) return FALSE;

    /* Handle quoted numbers from JS */
    if (*p == L'"') p++;
    *out = _wtoi(p);
    return TRUE;
}

BOOL json_get_bool(const wchar_t *json, const wchar_t *key, BOOL *out)
{
    const wchar_t *p = json_find_value(json, key);
    if (!p || !out) return FALSE;
    *out = (*p == L't' || *p == L'T') ? TRUE : FALSE;
    return TRUE;
}
