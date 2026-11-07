/*
 * ui.c - Main window + WebView2 UI bridge.
 *
 * Thin UI shell: WebView2 hosting, JSON message dispatch, display windows,
 * tray icon. All VM orchestration is in asb_core.c (the core library).
 */

#include "ui.h"
#include "asb_core.h"
#include "asb_events.h"
#include "headless.h"
#include "resource.h"
#include "hcs_vm.h"
#include "hcn_network.h"
#include "snapshot.h"
#include "vm_display.h"
#include "vm_display_idd.h"
#include "vm_agent.h"
#include "webview2_bridge.h"
#include "prereq.h"
#include "asb_paths.h"
#include <dwmapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <shlobj.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* Version and source repository, from Directory.Build.props / the CI build
   (bare tokens, stringized here as asb_core.c and headless.c do). */
#define UI_STR_(x)    #x
#define UI_WIDEN_(s)  L ## s
#define UI_WIDEN(s)   UI_WIDEN_(s)
#define UI_WSTR(x)    UI_WIDEN(UI_STR_(x))
#ifndef ASB_VERSION_SHORT
#define ASB_VERSION_SHORT 0.0.0
#endif
#ifdef ASB_SOURCE_REPO
#define UI_PROJECT_REPO UI_WSTR(ASB_SOURCE_REPO)
#else
#define UI_PROJECT_REPO L"jamesstringer90/appsandbox"
#endif
#define UI_UPSTREAM_REPO L"jamesstringer90/appsandbox"

/* ---- UI-specific globals ---- */

static HWND g_hwnd_main = NULL;
/* Registered message from a second launch: show the management window. */
static UINT g_show_main_msg;
static HINSTANCE g_hInstance = NULL;

/* Minimum client size in CSS pixels (DIPs), reported by the page; converted
   to physical pixels with the window's current DPI when Windows asks. */
static int g_min_css_width = 0;
static int g_min_css_height = 0;

/* Window size at 100% scaling; scaled by the monitor's DPI and limited to
   90% of its work area. */
#define UI_DEFAULT_WIDTH  1575
#define UI_DEFAULT_HEIGHT 900

/* Borderless fullscreen state. */
static BOOL g_borderless_fullscreen = FALSE;
static LONG_PTR g_windowed_style = 0;
static WINDOWPLACEMENT g_windowed_placement = { sizeof(WINDOWPLACEMENT) };

/* Display windows (indexed parallel to the library's VM array) */
static VmDisplay *g_displays[ASB_MAX_VMS];
static VmDisplayIdd *g_idd_displays[ASB_MAX_VMS];
static UINT64 g_display_vm_ids[ASB_MAX_VMS];
static unsigned g_core_subscription;
static LONG g_refresh_pending;
static int g_command_depth;
/* Nesting of main_wnd_proc calls, and of display teardowns that dispatch
   messages while they wait for a viewer thread. The exit sequence only runs
   from the outermost message dispatch (see exit_may_run). */
static int g_wndproc_depth;
static int g_pump_depth;
#define WM_CORE_CHANGED (WM_APP + 32)

/* Custom window messages */
#define WM_VM_STATE_CHANGED    (WM_APP + 1)
#define WM_VM_AGENT_STATUS     (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN   (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY    (WM_APP + 4)
#define WM_VM_DISPLAY_CLOSED   (WM_APP + 5)
#define WM_VM_MONITOR_DETECTED (WM_APP + 6)
#define WM_VM_IDD_READY        (WM_APP + 7)
#define WM_VM_HYPERV_VIDEO_OFF (WM_APP + 12)
#define WM_VM_REMOVED          (WM_APP + 16)
#define WM_WEBVIEW2_LOG        (WM_APP + 10)
#define WM_TRAYICON            (WM_APP + 11)
#define WM_SHOW_ALERT          (WM_APP + 15)
#define WM_VM_SHUTDOWN_TIMEOUT (WM_APP + 9)
#define WM_PREREQ_DONE        (WM_APP + 17)
#define WM_DISK_SPACE         (WM_APP + 19)
#define WM_CORE_READY         (WM_APP + 20)  /* wp: 1 core started, 0 prerequisites missing */
#define WM_BG_OP_DONE         (WM_APP + 21)  /* lp: BgOp* finished on the worker thread */

/* Exit sequence: VMs were asked to shut down and the app exits once they
   have stopped (tray Exit, closing the window, or Windows signing out). */
#define IDT_EXIT_WAIT          0x4C58
#define EXIT_WAIT_LIMIT_MS     (120 * 1000)
static BOOL      g_exit_waiting;
static BOOL      g_exit_for_session;     /* Windows is signing out, restarting or updating the app */
static BOOL      g_exit_forced;          /* Force stop was chosen: exit when nothing runs */
static BOOL      g_exit_block_reason;    /* ShutdownBlockReasonCreate is active */
static ULONGLONG g_exit_started;
static UINT64    g_exit_asked[ASB_MAX_VMS];    /* shutdown already requested */
static int       g_exit_asked_count;
static UINT64    g_exit_refused[ASB_MAX_VMS];  /* the request was not accepted */
static int       g_exit_refused_count;

/* Starting the core (prerequisite check, HCS/HCN, GPU enumeration through
   WMI, certificates, loading and re-attaching VMs) takes seconds, so it runs
   on a worker thread and the window stays responsive meanwhile. */
static volatile LONG g_core_starting;

/* Tray */
#define TRAY_CMD_SHOW          1
#define TRAY_CMD_EXIT          2
#define TRAY_CMD_CONNECT_BASE  100
#define TRAY_CMD_SHUTDOWN_BASE 200
#define TRAY_CMD_STOP_BASE     300

static NOTIFYICONDATAW g_nid;

/* UI thread ID for thread-safe log dispatch */
static DWORD g_ui_thread_id;

/* TRUE on Windows Home / Home N editions (Hyper-V not supported) */
static BOOL g_is_home_edition = FALSE;
static BOOL g_prereq_ok = FALSE;
static BOOL g_prereq_reboot_pending = FALSE;

/* ---- VM operations that wait on HCS ----
   Force stop, delete and shut down can wait on vmcompute for many seconds.
   They run on one worker thread so the window keeps responding. While one
   runs, ui_command_active() is TRUE (the local API answers "busy") and
   commands that change VMs are refused with a notice; the VM list keeps
   refreshing. */
typedef enum {
    BG_SHUTDOWN = 1,   /* one VM, from its card, detail view or the tray */
    BG_STOP,           /* force stop one VM */
    BG_DELETE,         /* delete one VM (turned off first when running) */
    BG_SHUTDOWN_ALL,   /* exit sequence: ask every listed VM to shut down */
    BG_STOP_ALL        /* exit sequence: force stop every listed VM */
} BgOpKind;

typedef struct {
    BgOpKind kind;
    int      count;
    UINT64   ids[ASB_MAX_VMS];
    wchar_t  names[ASB_MAX_VMS][128];
    HRESULT  results[ASB_MAX_VMS];
} BgOp;

static BgOp  *g_bg_op;       /* the operation in progress, owned by the UI thread */
static HANDLE g_bg_thread;

/* Local API requests that arrived while an operation ran: they wait for it,
   as they waited while such operations ran on the UI thread. */
static LPARAM g_deferred_api[8];
static int    g_deferred_api_count;
static BOOL   g_destroying;

/* Per-VM message shown on its card and in its detail view (a start that
   failed, a shutdown that was not accepted, ...). Cleared when the user
   dismisses it or starts the VM again. */
typedef struct {
    UINT64  id;
    wchar_t text[512];
} VmNote;
static VmNote g_vm_notes[ASB_MAX_VMS];

/* Last state seen per VM, to notice a disk build or start that ended
   without the VM running (the core reports those only in the log). */
typedef struct {
    UINT64  id;
    wchar_t name[128];
    BOOL    building;
    BOOL    starting;
} VmSeen;
static VmSeen g_vm_seen[ASB_MAX_VMS];
static int    g_vm_seen_count;

/* Last log line that reads as an error, used as the reason in those messages. */
static wchar_t   g_last_error_line[600];
static ULONGLONG g_last_error_tick;

/* While set, alerts raised on the UI thread are collected here instead of
   being shown (create and start report them inline). */
static wchar_t *g_alert_capture;
static size_t   g_alert_capture_cap;

static BOOL detect_home_edition(void)
{
    DWORD product_type = 0;
    if (GetProductInfo(10, 0, 0, 0, &product_type)) {
        switch (product_type) {
        case 0x00000065: /* PRODUCT_CORE */
        case 0x00000062: /* PRODUCT_CORE_N */
        case 0x00000064: /* PRODUCT_CORE_SINGLELANGUAGE */
        case 0x00000063: /* PRODUCT_CORE_COUNTRYSPECIFIC */
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Forward declarations ---- */

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void send_vm_list(void);
static int vm_icon_version(const VmInstance *v);
static void send_full_state(void);
static void send_adapters(void);
static void send_templates(void);
static void toggle_borderless_fullscreen(HWND hwnd);
static void on_webview2_accelerator(UINT virtual_key);
static void ui_show_alert(const wchar_t *message);
static void ui_alert(const wchar_t *kind, const wchar_t *title, const wchar_t *message);
static void ui_notice(const wchar_t *kind, const wchar_t *message);
static void exit_tick(HWND hwnd);
static BOOL exit_may_run(void);
static void safe_destroy_rdp(int idx);
static void safe_destroy_idd(int idx);
static void reconcile_displays(void);

static void toggle_borderless_fullscreen(HWND hwnd)
{
    if (!g_borderless_fullscreen) {
        MONITORINFO monitor = { sizeof(monitor) };
        HMONITOR hmonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        if (!GetWindowPlacement(hwnd, &g_windowed_placement) ||
            !GetMonitorInfoW(hmonitor, &monitor))
            return;

        g_windowed_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          (g_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_borderless_fullscreen = TRUE;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowed_style);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        g_borderless_fullscreen = FALSE;
    }
}

static void on_webview2_accelerator(UINT virtual_key)
{
    if (virtual_key == VK_F11 && g_hwnd_main)
        toggle_borderless_fullscreen(g_hwnd_main);
}

/* ---- Safe display teardown ---- */

static void safe_destroy_rdp(int idx)
{
    if (idx >= 0 && idx < ASB_MAX_VMS && g_displays[idx]) {
        VmDisplay *d = g_displays[idx];
        g_displays[idx] = NULL;
        ++g_pump_depth;
        vm_display_disconnect(d);
        vm_display_destroy(d);
        --g_pump_depth;
    }
}

static void safe_destroy_idd(int idx)
{
    if (idx >= 0 && idx < ASB_MAX_VMS && g_idd_displays[idx]) {
        VmDisplayIdd *d = g_idd_displays[idx];
        g_idd_displays[idx] = NULL;
        ++g_pump_depth;
        vm_display_idd_destroy(d);
        --g_pump_depth;
    }
}

static void reconcile_displays(void)
{
    VmDisplay *rdp[ASB_MAX_VMS] = {0};
    VmDisplayIdd *idd[ASB_MAX_VMS] = {0};
    UINT64 ids[ASB_MAX_VMS] = {0};
    int i;
    for (i = 0; i < ASB_MAX_VMS; ++i) {
        VmInstance *vm = asb_find_vm_by_id(g_display_vm_ids[i]);
        int idx = vm ? asb_vm_index((AsbVm)vm) : -1;
        if (idx < 0 || !vm->running) {
            safe_destroy_rdp(i);
            safe_destroy_idd(i);
        } else {
            rdp[idx] = g_displays[i];
            idd[idx] = g_idd_displays[i];
            ids[idx] = vm->unique_id;
        }
    }
    memcpy(g_displays, rdp, sizeof(rdp));
    memcpy(g_idd_displays, idd, sizeof(idd));
    memcpy(g_display_vm_ids, ids, sizeof(ids));
}

/* TRUE while a command runs on the UI thread or a VM operation runs on the
   worker thread; the local API then answers "busy" (API requests normally
   wait for a worker operation instead, see WM_ASB_API_REQUEST). */
BOOL ui_command_active(void) { return g_command_depth != 0 || g_bg_op != NULL; }
void ui_refresh_from_core(void)
{
    if (g_hwnd_main && InterlockedCompareExchange(&g_refresh_pending, 1, 0) == 0)
        if (!PostMessageW(g_hwnd_main, WM_CORE_CHANGED, 0, 0))
            InterlockedExchange(&g_refresh_pending, 0);
}

/* ---- JSON state builders ---- */

static int query_disk_free_gb(const wchar_t *selected)
{
    wchar_t supplied[MAX_PATH], path[MAX_PATH + 1];
    ULARGE_INTEGER available;
    ULONGLONG gb;
    DWORD length, attrs;
    size_t i, len;

    if (!selected || !selected[0]) {
        asb_default_disk_directory(supplied, MAX_PATH);
    } else {
        if (wcslen(selected) >= MAX_PATH) return -1;
        wcscpy_s(supplied, MAX_PATH, selected);
    }
    len = wcslen(supplied);
    for (i = 0; i < len; i++) {
        if (supplied[i] == L'/') supplied[i] = L'\\';
        if (supplied[i] < 32 || wcschr(L"*?\"<>|", supplied[i]) ||
            (supplied[i] == L':' && i != 1))
            return -1;
    }
    if (!((len >= 3 && ((supplied[0] >= L'A' && supplied[0] <= L'Z') ||
                        (supplied[0] >= L'a' && supplied[0] <= L'z')) &&
                        supplied[1] == L':' && supplied[2] == L'\\') ||
          (len >= 5 && supplied[0] == L'\\' && supplied[1] == L'\\')))
        return -1;
    length = GetFullPathNameW(supplied, MAX_PATH, path, NULL);
    if (!length || length >= MAX_PATH) return -1;
    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    /* UNC volume queries require a trailing separator. */
    if (path[length - 1] != L'\\') {
        path[length++] = L'\\';
        path[length] = 0;
    }
    if (!GetDiskFreeSpaceExW(path, &available, NULL, NULL)) return -1;
    gb = available.QuadPart / (1024ULL * 1024 * 1024);
    return gb > INT_MAX ? INT_MAX : (int)gb;
}

static void send_disk_space(const wchar_t *json, BOOL query)
{
    size_t input_len = wcslen(json);
    wchar_t *path, *response;
    JsonBuilder jb;
    int request_id = 0, free_gb = -1;

    /* Preserve the requested path in the reply even when it exceeds MAX_PATH
       and is therefore rejected by the filesystem query. */
    if (input_len > (((size_t)-1) / sizeof(wchar_t) - 128) / 2) return;
    path = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                               (input_len + 1) * sizeof(wchar_t));
    response = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                   (input_len * 2 + 128) * sizeof(wchar_t));
    if (!path || !response) {
        if (path) HeapFree(GetProcessHeap(), 0, path);
        if (response) HeapFree(GetProcessHeap(), 0, response);
        return;
    }
    if (json_get_string(json, L"path", path, input_len + 1) && query)
        free_gb = query_disk_free_gb(path);
    json_get_int(json, L"requestId", &request_id);
    jb_init(&jb, response, input_len * 2 + 128);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"diskSpace");
    jb_string(&jb, L"path", path);
    jb_int(&jb, L"requestId", request_id);
    jb_int(&jb, L"freeGb", free_gb);
    jb_object_end(&jb);
    /* WebView2 is apartment-bound; deliver the response on the UI thread. */
    if (!PostMessageW(g_hwnd_main, WM_DISK_SPACE, 0, (LPARAM)response))
        HeapFree(GetProcessHeap(), 0, response);
    HeapFree(GetProcessHeap(), 0, path);
}

static DWORD WINAPI disk_space_thread(LPVOID param)
{
    wchar_t *json = (wchar_t *)param;
    send_disk_space(json, TRUE);
    free(json);
    return 0;
}

/* "major.minor.patch.build" from this exe's VERSIONINFO (CI stamps the build
   number there), or the version the sources were compiled with. */
static const wchar_t *app_version(void)
{
    static wchar_t version[64];
    if (!version[0]) {
        wchar_t exe[MAX_PATH];
        DWORD unused = 0, size;
        if (GetModuleFileNameW(NULL, exe, MAX_PATH) &&
            (size = GetFileVersionInfoSizeW(exe, &unused)) != 0) {
            void *data = malloc(size);
            VS_FIXEDFILEINFO *info = NULL;
            UINT len = 0;
            if (data && GetFileVersionInfoW(exe, 0, size, data) &&
                VerQueryValueW(data, L"\\", (void **)&info, &len) && info && len >= sizeof(*info))
                swprintf_s(version, ARRAYSIZE(version), L"%u.%u.%u.%u",
                           HIWORD(info->dwProductVersionMS), LOWORD(info->dwProductVersionMS),
                           HIWORD(info->dwProductVersionLS), LOWORD(info->dwProductVersionLS));
            free(data);
        }
        if (!version[0]) wcscpy_s(version, ARRAYSIZE(version), UI_WSTR(ASB_VERSION_SHORT));
    }
    return version;
}

/* appsandbox.log is written relative to the current directory (the exe's
   folder for a zip install, %ProgramData%\AppSandbox for the Microsoft Store
   version; see main.c). */
static void log_file_path(wchar_t *out, size_t cap)
{
    DWORD n = GetFullPathNameW(L"appsandbox.log", (DWORD)cap, out, NULL);
    if (!n || n >= cap) out[0] = 0;
}

static void app_directory(wchar_t *out, size_t cap)
{
    wchar_t *slash;
    if (!GetModuleFileNameW(NULL, out, (DWORD)cap)) { out[0] = 0; return; }
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

static void build_host_info_json(JsonBuilder *jb)
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    GpuList *gpu_list = asb_gpu_list();
    DWORD host_cores, host_ram_mb;
    DWORD vm_cores = 0, vm_ram_mb = 0, vm_hdd_gb = 0;
    wchar_t base_dir[MAX_PATH];
    int free_gb;
    int i, count = asb_vm_count();

    GetSystemInfo(&si);
    host_cores = si.dwNumberOfProcessors;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    host_ram_mb = (DWORD)(ms.ullTotalPhys / (1024 * 1024));

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (v && v->running) { vm_cores += v->cpu_cores; vm_ram_mb += v->ram_mb; }
        if (v) vm_hdd_gb += v->hdd_gb;
    }

    asb_default_disk_directory(base_dir, MAX_PATH);
    free_gb = query_disk_free_gb(base_dir);

    jb_int(jb, L"hostCores", (int)host_cores);
    jb_int(jb, L"hostRamMb", (int)host_ram_mb);
    jb_int(jb, L"vmCores", (int)vm_cores);
    jb_int(jb, L"vmRamMb", (int)vm_ram_mb);
    jb_int(jb, L"freeGb", free_gb);
    jb_int(jb, L"vmHddGb", (int)vm_hdd_gb);
    jb_string(jb, L"defaultDiskDirectory", base_dir);
    jb_bool(jb, L"closeToTrayWithoutAsking", asb_get_suppress_tray_warn());
    jb_bool(jb, L"autoUpdateGuestTools", asb_get_auto_update_guest_tools());
    jb_bool(jb, L"viewerTitleStats", asb_get_viewer_title_stats());
    jb_bool(jb, L"viewerToolbar", asb_get_viewer_toolbar());
    jb_int(jb, L"viewerRenderer", asb_get_viewer_renderer());
    jb_bool(jb, L"hevcDecoder", vm_display_idd_hevc_supported());
    {
        wchar_t path[MAX_PATH];
        jb_string(jb, L"appVersion", app_version());
        jb_bool(jb, L"packaged", asb_running_packaged());
        jb_string(jb, L"projectRepo", UI_PROJECT_REPO);
        jb_string(jb, L"upstreamRepo", UI_UPSTREAM_REPO);
        asb_program_data_dir(path, MAX_PATH);
        jb_string(jb, L"dataDirectory", path);
        log_file_path(path, MAX_PATH);
        jb_string(jb, L"logFile", path);
        app_directory(path, MAX_PATH);
        jb_string(jb, L"appDirectory", path);
    }
    jb_array_begin(jb, L"gpus");
    for (i = 0; gpu_list && i < gpu_list->count; i++) {
        if (i > 0) jb_append(jb, L",");
        jb_object_begin(jb);
        jb_string(jb, L"id", gpu_list->gpus[i].interface_path);
        jb_string(jb, L"name", gpu_list->gpus[i].name);
        jb_string(jb, L"location", gpu_list->gpus[i].location);
        jb_object_end(jb);
    }
    jb_array_end(jb);
}

static ULONGLONG get_file_size_bytes(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER sz;
    if (!path || !path[0]) return 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    sz.LowPart = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    return sz.QuadPart;
}

static void jb_size_gb(JsonBuilder *jb, const wchar_t *key, ULONGLONG bytes)
{
    wchar_t buf[32];
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 10.0)
        swprintf_s(buf, 32, L"%.0f", gb);
    else
        swprintf_s(buf, 32, L"%.1f", gb);
    jb_string(jb, key, buf);
}

/* The VM's own folder: the one holding its disk, or the parent of its
   "snapshots" folder when it runs from a snapshot branch (as the core's
   get_vm_disk_root). FALSE for a drive root or an unknown disk path. */
static BOOL vm_root_folder(const VmInstance *v, wchar_t *out, size_t cap)
{
    wchar_t *slash;
    size_t len;
    BOOL branch;
    if (!v || !v->vhdx_path[0] || wcslen(v->vhdx_path) >= cap) return FALSE;
    wcscpy_s(out, cap, v->vhdx_path);
    slash = wcsrchr(out, L'\\');
    if (!slash) return FALSE;
    branch = _wcsnicmp(slash + 1, L"branch_", 7) == 0 || _wcsnicmp(slash + 1, L"snapshot_", 9) == 0;
    *slash = 0;
    len = wcslen(out);
    if (branch && len >= 10 && _wcsicmp(out + len - 10, L"\\snapshots") == 0)
        out[len - 10] = 0;
    return wcslen(out) > 3;
}

static const wchar_t *vm_note_get(UINT64 id)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (id && g_vm_notes[i].id == id) return g_vm_notes[i].text;
    return NULL;
}

static void vm_note_clear(UINT64 id)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (id && g_vm_notes[i].id == id) ZeroMemory(&g_vm_notes[i], sizeof(g_vm_notes[i]));
}

static void vm_note_set(UINT64 id, const wchar_t *text)
{
    int i, slot = -1;
    if (!id || !text) return;
    for (i = 0; i < ASB_MAX_VMS && slot < 0; i++)
        if (g_vm_notes[i].id == id) slot = i;
    for (i = 0; i < ASB_MAX_VMS && slot < 0; i++)
        if (!g_vm_notes[i].id) slot = i;
    if (slot < 0) {
        /* Notes of deleted VMs: reuse the slot of one that no longer exists. */
        for (i = 0; i < ASB_MAX_VMS && slot < 0; i++)
            if (!asb_find_vm_by_id(g_vm_notes[i].id)) slot = i;
    }
    if (slot < 0) slot = 0;
    g_vm_notes[slot].id = id;
    wcsncpy_s(g_vm_notes[slot].text, ARRAYSIZE(g_vm_notes[slot].text), text, _TRUNCATE);
}

/* "stopping" / "deleting" / "shuttingDown" while the worker thread runs that
   operation for this VM, else NULL. */
static const wchar_t *vm_pending_op(UINT64 id)
{
    int i;
    if (!g_bg_op) return NULL;
    for (i = 0; i < g_bg_op->count; i++) {
        if (g_bg_op->ids[i] != id) continue;
        switch (g_bg_op->kind) {
        case BG_STOP: case BG_STOP_ALL: return L"stopping";
        case BG_DELETE: return L"deleting";
        default: return L"shuttingDown";
        }
    }
    return NULL;
}

static void build_vm_json(JsonBuilder *jb, int i)
{
    wchar_t disk_directory[MAX_PATH];
    VmInstance *v = asb_vm_instance(asb_vm_get(i));
    SnapshotTree *st_ = asb_vm_snap_tree(asb_vm_get(i));
    if (!v || !st_) return;

    jb_object_begin(jb);
    wchar_t id[ASB_VM_ID_TEXT_SIZE];
    asb_vm_id_format_w(v->unique_id, id);
    jb_string(jb, L"id", id);
    jb_string(jb, L"name", v->name);
    jb_string(jb, L"osType", v->os_type);
    jb_int(jb, L"iconVersion", vm_icon_version(v));
    jb_bool(jb, L"hasIso", v->image_path[0] != L'\0');
    asb_vm_disk_directory(asb_vm_get(i), disk_directory, MAX_PATH);
    jb_string(jb, L"diskDirectory", disk_directory);
    jb_bool(jb, L"running", v->running);
    jb_bool(jb, L"starting", v->starting);
    jb_bool(jb, L"shuttingDown", v->shutdown_requested);
    jb_bool(jb, L"agentOnline", v->agent_online);
    jb_int(jb, L"ramMb", (int)v->ram_mb);
    jb_int(jb, L"hddGb", (int)v->hdd_gb);
    jb_int(jb, L"cpuCores", (int)v->cpu_cores);
    jb_int(jb, L"gpuMode", v->gpu_mode);
    jb_string(jb, L"gpuId", v->gpu_id);
    jb_string(jb, L"gpuName", v->gpu_name);
    jb_int(jb, L"networkMode", v->network_mode);
    jb_string(jb, L"netAdapter", v->net_adapter);
    jb_bool(jb, L"isTemplate", v->is_template);
    jb_bool(jb, L"hypervVideoOff", v->hyperv_video_off);
    jb_bool(jb, L"buildingVhdx", v->building_vhdx);
    jb_bool(jb, L"vhdxStaging", v->vhdx_staging);
    jb_int(jb, L"vhdxProgress", v->vhdx_progress);
    jb_bool(jb, L"installComplete", v->install_complete);
    jb_bool(jb, L"sshEnabled", v->ssh_enabled);
    jb_int(jb, L"sshPort", (int)v->ssh_port);
    jb_int(jb, L"sshState", (v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state);
    jb_bool(jb, L"sshDeployKey", v->ssh_deploy_key);
    jb_bool(jb, L"sshKeyDeployed", v->ssh_key_deployed);
    jb_bool(jb, L"copyingDisk", v->copying_disk);
    jb_int(jb, L"copyProgress", (int)v->copy_progress);
    {
        wchar_t folder[MAX_PATH], nat_ip[32];
        const wchar_t *note = vm_note_get(v->unique_id), *pending = vm_pending_op(v->unique_id);
        int k;
        jb_string(jb, L"vmFolder", vm_root_folder(v, folder, MAX_PATH) ? folder : L"");
        /* NAT address (dotted ASCII) for VMs that need it set by hand. */
        for (k = 0; k < (int)ARRAYSIZE(nat_ip) - 1 && k < (int)sizeof(v->nat_ip) &&
                    ((v->nat_ip[k] >= '0' && v->nat_ip[k] <= '9') || v->nat_ip[k] == '.'); k++)
            nat_ip[k] = (wchar_t)v->nat_ip[k];
        nat_ip[k] = 0;
        jb_string(jb, L"natIp", v->network_mode == ASB_NET_NAT ? nat_ip : L"");
        jb_string(jb, L"lastError", note ? note : L"");
        jb_string(jb, L"pendingOp", pending ? pending : L"");
    }

    /* Display mode (Windows and Linux guests; the IDD window owns the settings file). */
    if (_wcsicmp(v->os_type, L"Windows") == 0 || _wcsicmp(v->os_type, L"Linux") == 0) {
        wchar_t res[32];
        UINT hz = 60;
        int k, driver = -1;
        vm_display_idd_saved_mode(v->vhdx_path, res, ARRAYSIZE(res), &hz);
        jb_string(jb, L"displayResolution", res);
        jb_int(jb, L"displayHz", (int)hz);
        for (k = 0; k < ASB_MAX_VMS; k++)
            if (g_display_vm_ids[k] == v->unique_id && g_idd_displays[k])
                driver = vm_display_idd_guest_driver(g_idd_displays[k]);
        jb_int(jb, L"displayDriver", driver);
        {
            int video = -1;
            for (k = 0; k < ASB_MAX_VMS; k++)
                if (g_display_vm_ids[k] == v->unique_id && g_idd_displays[k])
                    video = vm_display_idd_video_active(g_idd_displays[k]);
            const wchar_t *vcodec = NULL;
            for (k = 0; k < ASB_MAX_VMS; k++)
                if (g_display_vm_ids[k] == v->unique_id && g_idd_displays[k])
                    vcodec = vm_display_idd_video_codec(g_idd_displays[k]);
            jb_bool(jb, L"displayPerformance", vm_display_idd_saved_performance(v->vhdx_path));
            jb_int(jb, L"displayVideo", video);
            jb_string(jb, L"displayCodec", vm_display_idd_saved_codec(v->vhdx_path));
            jb_string(jb, L"displayVideoCodec", vcodec ? vcodec : L"");
            jb_int(jb, L"displayBitrate", (int)vm_display_idd_saved_bitrate(v->vhdx_path));
        }
        jb_bool(jb, L"updatingTools", v->updating_tools);
        jb_bool(jb, L"guestToolsOutdated", asb_vm_guest_tools_outdated(asb_vm_get(i)));
    }

    /* Snapshot tree */
    {
        int s, b;
        int cur_snap, cur_branch;
        snapshot_find_current(st_, v->vhdx_path, &cur_snap, &cur_branch);

        jb_int(jb, L"snapCurrent", cur_snap);
        jb_int(jb, L"snapCurrentBranch", cur_branch);
        jb_bool(jb, L"hasSnapshots", st_->base_vhdx[0] != L'\0');

        {
            ULONGLONG base_size = get_file_size_bytes(st_->base_vhdx);

            /* Base branches */
            jb_array_begin(jb, L"baseBranches");
            for (b = 0; b < st_->base_branch_count; b++) {
                FILETIME bft;
                if (!st_->base_branches[b].valid) continue;
                if (b > 0) jb_append(jb, L",");
                jb_object_begin(jb);
                jb_string(jb, L"name", st_->base_branches[b].friendly_name);
                if (snapshot_get_branch_time(st_, -2, b, &bft)) {
                    FILETIME lft; SYSTEMTIME st;
                    wchar_t db[64];
                    FileTimeToLocalFileTime(&bft, &lft);
                    FileTimeToSystemTime(&lft, &st);
                    swprintf_s(db, 64, L"%04d-%02d-%02d %02d:%02d",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                    jb_string(jb, L"date", db);
                }
                jb_size_gb(jb, L"sizeGb", base_size + get_file_size_bytes(st_->base_branches[b].vhdx_path));
                jb_object_end(jb);
            }
            jb_array_end(jb);

            /* Snapshots */
            jb_array_begin(jb, L"snapshots");
            for (s = 0; s < st_->count; s++) {
                FILETIME local_ft;
                SYSTEMTIME sys_t;
                wchar_t date_buf[64];
                if (!st_->nodes[s].valid) continue;
                FileTimeToLocalFileTime(&st_->nodes[s].created, &local_ft);
                FileTimeToSystemTime(&local_ft, &sys_t);
                swprintf_s(date_buf, 64, L"%04d-%02d-%02d %02d:%02d",
                    sys_t.wYear, sys_t.wMonth, sys_t.wDay, sys_t.wHour, sys_t.wMinute);
                if (s > 0) jb_append(jb, L",");
                jb_object_begin(jb);
                jb_string(jb, L"name", st_->nodes[s].name);
                jb_string(jb, L"date", date_buf);
                jb_int(jb, L"parent", snapshot_parent_index(st_, s));

                {
                    /* A snapshot taken on another one sits on that one's disk too */
                    ULONGLONG snap_size = 0;
                    int p, hops;
                    for (p = s, hops = 0; p >= 0 && hops < MAX_SNAPSHOTS;
                         p = snapshot_parent_index(st_, p), hops++)
                        snap_size += get_file_size_bytes(st_->nodes[p].snap_vhdx);
                    jb_array_begin(jb, L"branches");
                    for (b = 0; b < st_->nodes[s].branch_count; b++) {
                        FILETIME bft;
                        if (!st_->nodes[s].branches[b].valid) continue;
                        if (b > 0) jb_append(jb, L",");
                        jb_object_begin(jb);
                        jb_string(jb, L"name", st_->nodes[s].branches[b].friendly_name);
                        if (snapshot_get_branch_time(st_, s, b, &bft)) {
                            FILETIME lft; SYSTEMTIME bst;
                            wchar_t db[64];
                            FileTimeToLocalFileTime(&bft, &lft);
                            FileTimeToSystemTime(&lft, &bst);
                            swprintf_s(db, 64, L"%04d-%02d-%02d %02d:%02d",
                                bst.wYear, bst.wMonth, bst.wDay, bst.wHour, bst.wMinute);
                            jb_string(jb, L"date", db);
                        }
                        jb_size_gb(jb, L"sizeGb", base_size + snap_size + get_file_size_bytes(st_->nodes[s].branches[b].vhdx_path));
                        jb_object_end(jb);
                    }
                    jb_array_end(jb);
                }

                jb_object_end(jb);
            }
            jb_array_end(jb);
        }
    }

    jb_object_end(jb);
}

/* " Reason: <last error log line>" when one arrived in the last 30 s. */
static void recent_error_reason(wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (g_last_error_line[0] && GetTickCount64() - g_last_error_tick < 30000)
        _snwprintf_s(out, cap, _TRUNCATE, L" Reason: %s", g_last_error_line);
}

/* A disk build or start that ends without the VM running is reported by the
   core only in the log, or with an alert that names no VM. Compare with the
   previous refresh: a VM that stopped instead of starting gets a note on its
   card; a VM whose disk build removed it gets a message. */
static void track_vm_transitions(void)
{
    VmSeen now[ASB_MAX_VMS];
    int i, j, n = 0, count = asb_vm_count();

    for (i = 0; i < count && n < ASB_MAX_VMS; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v || !v->unique_id) continue;
        now[n].id = v->unique_id;
        wcsncpy_s(now[n].name, ARRAYSIZE(now[n].name), v->name, _TRUNCATE);
        now[n].building = v->building_vhdx;
        now[n].starting = v->starting;
        for (j = 0; j < g_vm_seen_count; j++) {
            if (g_vm_seen[j].id != v->unique_id) continue;
            if ((g_vm_seen[j].building || g_vm_seen[j].starting) &&
                !v->building_vhdx && !v->starting && !v->running &&
                !v->copying_disk && !v->updating_tools) {
                wchar_t reason[700], text[512];
                recent_error_reason(reason, ARRAYSIZE(reason));
                _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                             L"Did not start.%s Details are in Activity.", reason);
                vm_note_set(v->unique_id, text);
            }
            break;
        }
        n++;
    }
    for (j = 0; j < g_vm_seen_count; j++) {
        BOOL present = FALSE;
        if (!g_vm_seen[j].building) continue;
        for (i = 0; i < n && !present; i++) present = now[i].id == g_vm_seen[j].id;
        if (!present) {
            wchar_t reason[700], text[1024];
            recent_error_reason(reason, ARRAYSIZE(reason));
            _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                         L"\"%s\" could not be created, so it was removed from the list.%s "
                         L"Details are in Activity.", g_vm_seen[j].name, reason);
            ui_alert(L"error", L"Could not create the VM", text);
        }
    }
    if (n) memcpy(g_vm_seen, now, n * sizeof(now[0]));
    g_vm_seen_count = n;
}

static void send_vm_list(void)
{
    wchar_t buf[131072];
    JsonBuilder jb;
    int i, count;

    track_vm_transitions();
    count = asb_vm_count();
    jb_init(&jb, buf, ARRAYSIZE(buf));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"vmListChanged");

    jb_array_begin(&jb, L"vms");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(&jb, L",");
        build_vm_json(&jb, i);
    }
    jb_array_end(&jb);

    {
        wchar_t hi_buf[32768];
        JsonBuilder hi;
        jb_init(&hi, hi_buf, ARRAYSIZE(hi_buf));
        jb_object_begin(&hi);
        build_host_info_json(&hi);
        jb_object_end(&hi);
        if (jb.count > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"hostInfo\":");
        jb_append(&jb, hi_buf);
        jb.count++;
    }

    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_host_info(void)
{
    wchar_t buf[32768];
    JsonBuilder jb;
    jb_init(&jb, buf, ARRAYSIZE(buf));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"hostInfo");
    build_host_info_json(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

/* IF_TYPE constants */
#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD 6
#endif
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif

typedef struct {
    wchar_t names[32][256];
    int count;
    int first_eth;
    int first_wifi;
} AdapterList;

static void adapter_enum_cb(const wchar_t *name, int if_type, void *ctx)
{
    AdapterList *al = (AdapterList *)ctx;
    if (al->count >= 32) return;
    wcscpy_s(al->names[al->count], 256, name);
    if (if_type == IF_TYPE_ETHERNET_CSMACD && al->first_eth < 0)
        al->first_eth = al->count + 1;
    else if (if_type == IF_TYPE_IEEE80211 && al->first_wifi < 0)
        al->first_wifi = al->count + 1;
    al->count++;
}

static void send_adapters(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    AdapterList al;
    int def_idx, i;

    al.count = 0; al.first_eth = -1; al.first_wifi = -1;
    hcn_enum_adapters(adapter_enum_cb, &al);
    def_idx = (al.first_eth >= 0) ? al.first_eth : (al.first_wifi >= 0) ? al.first_wifi : 0;

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"adapters");
    jb_array_begin(&jb, L"adapters");
    for (i = 0; i < al.count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"");
        jb_append_escaped(&jb, al.names[i]);
        jb_append(&jb, L"\"");
    }
    jb_array_end(&jb);
    jb_int(&jb, L"defaultIndex", def_idx);
    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_templates(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    int i, count = asb_template_count();

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"templates");
    jb_array_begin(&jb, L"templates");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", asb_template_name(i));
        jb_string(&jb, L"osType", asb_template_os_type(i));
        jb_object_end(&jb);
    }
    jb_array_end(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

static int CALLBACK disk_folder_browse_callback(HWND hwnd, UINT message, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (message == BFFM_INITIALIZED && data)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void send_full_state(void)
{
    wchar_t buf[131072];
    JsonBuilder jb;
    int i, count;

    track_vm_transitions();
    jb_init(&jb, buf, ARRAYSIZE(buf));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"fullState");

    count = asb_vm_count();
    jb_array_begin(&jb, L"vms");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(&jb, L",");
        build_vm_json(&jb, i);
    }
    jb_array_end(&jb);

    /* Host info */
    {
        wchar_t hi[32768];
        JsonBuilder hj;
        jb_init(&hj, hi, ARRAYSIZE(hi));
        jb_object_begin(&hj);
        build_host_info_json(&hj);
        jb_object_end(&hj);
        if (jb.count > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"hostInfo\":");
        jb_append(&jb, hi);
        jb.count++;
    }

    /* Adapters */
    {
        AdapterList al;
        int def_idx;
        al.count = 0; al.first_eth = -1; al.first_wifi = -1;
        hcn_enum_adapters(adapter_enum_cb, &al);
        def_idx = (al.first_eth >= 0) ? al.first_eth : (al.first_wifi >= 0) ? al.first_wifi : 0;

        jb_array_begin(&jb, L"adapters");
        for (i = 0; i < al.count; i++) {
            if (i > 0) jb_append(&jb, L",");
            jb_append(&jb, L"\"");
            jb_append_escaped(&jb, al.names[i]);
            jb_append(&jb, L"\"");
        }
        jb_array_end(&jb);
        jb_int(&jb, L"defaultAdapter", def_idx);
    }

    /* Templates */
    {
        int tc = asb_template_count();
        jb_array_begin(&jb, L"templates");
        for (i = 0; i < tc; i++) {
            if (i > 0) jb_append(&jb, L",");
            jb_object_begin(&jb);
            jb_string(&jb, L"name", asb_template_name(i));
            jb_string(&jb, L"osType", asb_template_os_type(i));
            jb_object_end(&jb);
        }
        jb_array_end(&jb);
    }

    jb_object_end(&jb);
    webview2_post(buf);
}

/* ---- UI logging, alerts and notices ---- */

/* Lines that explain why something failed; the newest one is quoted in the
   messages built by track_vm_transitions. */
static BOOL log_line_is_error(const wchar_t *msg)
{
    while (*msg == L' ' || *msg == L'\t') msg++;
    return _wcsnicmp(msg, L"Error", 5) == 0 || wcsstr(msg, L"failed to start") != NULL ||
           wcsstr(msg, L"Failed to start") != NULL || wcsstr(msg, L"Shutdown failed") != NULL;
}

static void ui_log_post(const wchar_t *msg)
{
    wchar_t json[8192];
    JsonBuilder jb;
    if (log_line_is_error(msg)) {
        wcsncpy_s(g_last_error_line, ARRAYSIZE(g_last_error_line), msg, _TRUNCATE);
        g_last_error_tick = GetTickCount64();
    }
    jb_init(&jb, json, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"log");
    jb_string(&jb, L"message", msg);
    jb_object_end(&jb);
    webview2_post(json);
}

/* Append to a JSON string body, escaping quotes, backslashes and every
   control character (messages carry VM names, paths and core text). */
static void json_escape_append(wchar_t *out, size_t cap, size_t *len, const wchar_t *s)
{
    for (; s && *s; s++) {
        wchar_t c = *s;
        if (*len + 7 >= cap) break;
        if (c == L'"' || c == L'\\') { out[(*len)++] = L'\\'; out[(*len)++] = c; }
        else if (c == L'\n') { out[(*len)++] = L'\\'; out[(*len)++] = L'n'; }
        else if (c == L'\r') { out[(*len)++] = L'\\'; out[(*len)++] = L'r'; }
        else if (c == L'\t') { out[(*len)++] = L'\\'; out[(*len)++] = L't'; }
        else if (c < 32 || c == 0x2028 || c == 0x2029) {
            swprintf_s(out + *len, cap - *len, L"\\u%04x", (unsigned)c);
            *len += 6;
        }
        else out[(*len)++] = c;
    }
    out[*len] = 0;
}

static void json_raw_append(wchar_t *out, size_t cap, size_t *len, const wchar_t *s)
{
    size_t n = wcslen(s);
    if (*len + n >= cap) return;
    wmemcpy(out + *len, s, n);
    *len += n;
    out[*len] = 0;
}

/* Post {"type":type,"kind":kind,"title":title,"message":message}. */
static void post_text_message(const wchar_t *type, const wchar_t *kind,
                              const wchar_t *title, const wchar_t *message)
{
    size_t len = 0, cap;
    wchar_t *buf;
    if (!type) return;
    if (!kind) kind = L"";
    if (!title) title = L"";
    if (!message) message = L"";
    cap = (wcslen(type) + wcslen(kind) + wcslen(title) + wcslen(message)) * 6 + 128;
    buf = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!buf) return;
    buf[0] = 0;
    json_raw_append(buf, cap, &len, L"{\"type\":\"");
    json_escape_append(buf, cap, &len, type);
    json_raw_append(buf, cap, &len, L"\",\"kind\":\"");
    json_escape_append(buf, cap, &len, kind);
    json_raw_append(buf, cap, &len, L"\",\"title\":\"");
    json_escape_append(buf, cap, &len, title);
    json_raw_append(buf, cap, &len, L"\",\"message\":\"");
    json_escape_append(buf, cap, &len, message);
    json_raw_append(buf, cap, &len, L"\"}");
    webview2_post(buf);
    free(buf);
}

typedef struct {
    wchar_t kind[16];
    wchar_t title[128];
    wchar_t message[1];   /* allocated to fit */
} PendingAlert;

/* A dialog in the page with one OK button. kind: "error" (titled "Something
   went wrong" unless a title is given), "warning" or "info". Any thread. */
static void ui_alert(const wchar_t *kind, const wchar_t *title, const wchar_t *message)
{
    if (!message) return;
    if (GetCurrentThreadId() == g_ui_thread_id) {
        if (g_alert_capture) {
            /* create/start report these inline instead */
            size_t used = wcslen(g_alert_capture);
            if (used && used + 2 < g_alert_capture_cap) wcscat_s(g_alert_capture, g_alert_capture_cap, L"\n");
            used = wcslen(g_alert_capture);
            if (used + 1 < g_alert_capture_cap)
                wcsncat_s(g_alert_capture, g_alert_capture_cap, message, _TRUNCATE);
            return;
        }
        post_text_message(L"alert", kind ? kind : L"error", title, message);
    } else if (g_hwnd_main) {
        size_t len = wcslen(message);
        PendingAlert *pa = (PendingAlert *)calloc(1, sizeof(PendingAlert) + len * sizeof(wchar_t));
        if (pa) {
            wcsncpy_s(pa->kind, ARRAYSIZE(pa->kind), kind ? kind : L"error", _TRUNCATE);
            wcsncpy_s(pa->title, ARRAYSIZE(pa->title), title ? title : L"", _TRUNCATE);
            wcscpy_s(pa->message, len + 1, message);
            if (!PostMessageW(g_hwnd_main, WM_SHOW_ALERT, 0, (LPARAM)pa)) free(pa);
        }
    }
}

/* Alerts from the core (errors the user must see). */
static void ui_show_alert(const wchar_t *message)
{
    ui_alert(L"error", NULL, message);
}

/* A message in the page's notification area that does not interrupt (shortcut
   created, path copied, a command refused while another runs). UI thread. */
static void ui_notice(const wchar_t *kind, const wchar_t *message)
{
    post_text_message(L"notice", kind ? kind : L"info", NULL, message);
}

/* Collect UI-thread alerts while a create or start runs (see ui_alert). */
static void alert_capture_begin(wchar_t *buf, size_t cap)
{
    buf[0] = 0;
    g_alert_capture = buf;
    g_alert_capture_cap = cap;
}

static void alert_capture_end(void)
{
    g_alert_capture = NULL;
    g_alert_capture_cap = 0;
}

/* Plain-language reason for an HRESULT from a VM operation. */
static void describe_hresult(HRESULT hr, wchar_t *out, size_t cap)
{
    if (hr == E_ACCESSDENIED)
        wcscpy_s(out, cap, L"it is busy being created, started, updated, imported or exported. Try again when that finishes.");
    else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        wcscpy_s(out, cap, L"one of its files (disk or installer ISO) was not found.");
    else if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
        wcscpy_s(out, cap, L"its disk is in use by another program.");
    else if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL))
        wcscpy_s(out, cap, L"the disk is full.");
    else if (hr == E_OUTOFMEMORY || hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
             hr == HRESULT_FROM_WIN32(ERROR_COMMITMENT_LIMIT))
        wcscpy_s(out, cap, L"this PC does not have enough free memory.");
    else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
        wcscpy_s(out, cap, L"its guest agent is not connected, so it cannot be asked to shut down. Shut it down from inside the VM, or use Force stop.");
    else
        swprintf_s(out, cap, L"error 0x%08X.", (unsigned)hr);
}

/* ---- Display connections ---- */

/* Linux / legacy display path: connects to vmwp.exe's Basic Session named
   pipe (\\.\pipe\<vm-name>.BasicSession) and renders via MsRdpClient10
   ActiveX. Reads the synthetic Hyper-V Video adapter framebuffer — works
   for any guest from BIOS / GRUB onward, no guest cooperation required.
   Used as a side-debug window during install: while cloud-init is still
   bringing up the desktop and our IDD agent isn't up yet, this is the
   only way to see what the VM is actually doing. Coexists with IDD —
   they render from independent sources so both can stay open. */
static void do_connect_rdp(int idx)
{
    VmInstance *v;
    wchar_t pipe[300];
    int tries;
    if (idx < 0 || idx >= asb_vm_count()) return;
    v = asb_vm_instance(asb_vm_get(idx));
    if (!v || !v->running) { ui_log(L"VM \"%s\" is not running.", v ? v->name : L"?"); return; }
    if (g_displays[idx] && vm_display_is_open(g_displays[idx])) {
        ui_log(L"RDP display already open."); return;
    }
    safe_destroy_rdp(idx);

    /* vmwp.exe creates the pipe a couple of seconds after the VM starts;
       poll briefly so the first Connect click doesn't race the worker. */
    swprintf_s(pipe, 300, L"\\\\.\\pipe\\%s.BasicSession", v->name);
    for (tries = 0; tries < 50; tries++) {
        if (WaitNamedPipeW(pipe, 0)) break;
        {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_SEM_TIMEOUT) break;
        }
        Sleep(100);
    }

    ui_log(L"Opening RDP basic-session display for \"%s\"...", v->name);
    g_display_vm_ids[idx] = v->unique_id;
    g_displays[idx] = vm_display_create(v, g_hInstance, g_hwnd_main);
    if (!g_displays[idx]) ui_log(L"Error: Failed to create RDP display window.");
}

static void do_connect_idd(int idx)
{
    VmInstance *v;
    if (idx < 0 || idx >= asb_vm_count()) return;
    v = asb_vm_instance(asb_vm_get(idx));
    if (!v || !v->running) { ui_log(L"VM \"%s\" is not running.", v ? v->name : L"?"); return; }
    if (g_idd_displays[idx] && vm_display_idd_is_active(g_idd_displays[idx])) {
        vm_display_idd_focus(g_idd_displays[idx]);
        return;
    }
    safe_destroy_idd(idx);
    ui_log(L"Opening IDD display for \"%s\"...", v->name);
    g_display_vm_ids[idx] = v->unique_id;
    g_idd_displays[idx] = vm_display_idd_create(v, g_hInstance, g_hwnd_main);
    if (!g_idd_displays[idx]) ui_log(L"Error: Failed to create IDD display window.");
    asb_events_changed();
}

/* User-initiated Connect: open BOTH the IDD display and the RDP
   basic-session display side-by-side.
     - IDD renders the in-VM agent's captured framebuffer (Windows: VDD,
       Linux: appsandbox-display via vsock :2). Empty until the agent
       comes online.
     - RDP renders the Hyper-V Video adapter framebuffer via vmwp.exe's
       BasicSession named pipe. Works from BIOS / GRUB onward, no guest
       cooperation needed. Crucial for debugging install / first boot —
       you can watch cloud-init, kernel messages, GDM coming up.
   Both windows are independent; either can be closed manually. The
   automatic RDP -> IDD switching handlers (WM_VM_AGENT_ONLINE,
   WM_VM_HYPERV_VIDEO_OFF) keep using the per-display functions so they
   can close one without re-opening the other. */
static void do_connect_vm(int idx);

/* ---- Custom VM icons ----
   Stored beside the VM's disk: icon.png (as chosen, shown in the UI) and
   icon.ico (the same picture in an .ico wrapper, for the VM window,
   taskbar and shortcuts). */

#define VM_ICON_MAX_BYTES (1024 * 1024)

static void vm_icon_file(const VmInstance *v, const wchar_t *name, wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH], *slash;
    wcscpy_s(dir, MAX_PATH, v->vhdx_path);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf_s(out, cap, L"%s\\%s", dir, name);
}

/* Change counter for the UI: last write time of the icon, 0 = none. */
static int vm_icon_version(const VmInstance *v)
{
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!v || !v->vhdx_path[0]) return 0;
    vm_icon_file(v, L"icon.png", path, MAX_PATH);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        vm_icon_file(v, L"icon.ico", path, MAX_PATH);
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return 0;
    }
    return (int)((fa.ftLastWriteTime.dwLowDateTime ^ fa.ftLastWriteTime.dwHighDateTime) & 0x7FFFFFFF) | 1;
}

static BYTE *read_small_file(const wchar_t *path, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    BYTE *buf;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > VM_ICON_MAX_BYTES) { CloseHandle(f); return NULL; }
    buf = (BYTE *)malloc(size);
    if (buf && (!ReadFile(f, buf, size, &got, NULL) || got != size)) { free(buf); buf = NULL; }
    CloseHandle(f);
    *len = size;
    return buf;
}

static BOOL write_file_all(const wchar_t *path, const void *a, DWORD alen, const void *b, DWORD blen)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, a, alen, &w, NULL) && w == alen;
    if (ok && b && blen) ok = WriteFile(f, b, blen, &w, NULL) && w == blen;
    CloseHandle(f);
    return ok;
}

/* Set the VM's icon from a .png or .ico file. */
static BOOL vm_icon_set(const VmInstance *v, const wchar_t *src, wchar_t *err, size_t err_cap)
{
    wchar_t png[MAX_PATH], ico[MAX_PATH];
    DWORD len = 0;
    BYTE *data = read_small_file(src, &len);
    BOOL ok = FALSE;
    if (!data) {
        swprintf_s(err, err_cap, L"Choose a PNG or ICO file of 1 MB or less.");
        return FALSE;
    }
    vm_icon_file(v, L"icon.png", png, MAX_PATH);
    vm_icon_file(v, L"icon.ico", ico, MAX_PATH);
    if (len > 24 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        /* PNG: keep it, and wrap it in a one-image .ico (Vista+ ICO may hold PNG). */
        UINT w = ((UINT)data[16] << 24) | ((UINT)data[17] << 16) | ((UINT)data[18] << 8) | data[19];
        UINT h = ((UINT)data[20] << 24) | ((UINT)data[21] << 16) | ((UINT)data[22] << 8) | data[23];
        BYTE hdr[22];
        ZeroMemory(hdr, sizeof(hdr));
        hdr[2] = 1;                         /* type: icon */
        hdr[4] = 1;                         /* one image */
        hdr[6] = (BYTE)(w >= 256 ? 0 : w);  /* 0 means 256 or more */
        hdr[7] = (BYTE)(h >= 256 ? 0 : h);
        hdr[10] = 1;                        /* planes */
        hdr[12] = 32;                       /* bit count */
        memcpy(hdr + 14, &len, 4);          /* bytes in resource */
        hdr[18] = 22;                       /* offset of the image */
        ok = write_file_all(png, data, len, NULL, 0) && write_file_all(ico, hdr, 22, data, len);
    } else if (len > 6 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0) {
        DeleteFileW(png);                   /* ICO: the UI shows it as is */
        ok = write_file_all(ico, data, len, NULL, 0);
    } else {
        swprintf_s(err, err_cap, L"That file is not a PNG or ICO image.");
    }
    if (!ok && !err[0]) swprintf_s(err, err_cap, L"The icon could not be saved next to the VM's disk.");
    free(data);
    return ok;
}

/* Send the icon to the UI as a data URL. */
static void vm_icon_post(const VmInstance *v)
{
    wchar_t path[MAX_PATH], id[ASB_VM_ID_TEXT_SIZE];
    const wchar_t *mime = L"image/png";
    DWORD len = 0, b64len = 0;
    BYTE *data;
    wchar_t *b64, *msg;
    size_t cap;

    asb_vm_id_format_w(v->unique_id, id);
    vm_icon_file(v, L"icon.png", path, MAX_PATH);
    data = read_small_file(path, &len);
    if (!data) {
        vm_icon_file(v, L"icon.ico", path, MAX_PATH);
        data = read_small_file(path, &len);
        mime = L"image/x-icon";
    }
    if (!data) {
        wchar_t none[160];
        swprintf_s(none, ARRAYSIZE(none), L"{\"type\":\"vmIcon\",\"vmId\":\"%s\",\"data\":\"\"}", id);
        webview2_post(none);
        return;
    }
    if (!CryptBinaryToStringW(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64len)) {
        free(data);
        return;
    }
    b64 = (wchar_t *)malloc(b64len * sizeof(wchar_t));
    cap = b64len + 256;
    msg = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (b64 && msg && CryptBinaryToStringW(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len)) {
        swprintf_s(msg, cap, L"{\"type\":\"vmIcon\",\"vmId\":\"%s\",\"data\":\"data:%s;base64,%s\"}",
                   id, mime, b64);
        webview2_post(msg);
    }
    free(b64);
    free(msg);
    free(data);
}

/* ---- VM shortcuts (desktop / Start menu) ---- */

#define IDT_LAUNCH_VM 0x4C56
#define IDT_OPEN_CONSOLE 0x4C57
/* Manual-install VM whose console opens once it is running. */
static UINT64 g_console_vm_id;
static int    g_console_tries;

static void open_console_when_running(UINT64 vm_id)
{
    g_console_vm_id = vm_id;
    g_console_tries = 0;
    if (g_hwnd_main) SetTimer(g_hwnd_main, IDT_OPEN_CONSOLE, 1000, NULL);
}
static wchar_t g_launch_vm[256];
static int     g_launch_tries;

void ui_launch_vm_by_name(const wchar_t *name)
{
    if (!name || !name[0] || !g_hwnd_main) return;
    wcscpy_s(g_launch_vm, ARRAYSIZE(g_launch_vm), name);
    g_launch_tries = 0;
    SetTimer(g_hwnd_main, IDT_LAUNCH_VM, 300, NULL);
}

/* Runs until the app finished initializing (prerequisites, VM list), then
   starts the VM or brings its display forward. */
static void launch_vm_tick(HWND hwnd)
{
    int i, idx = -1;
    VmInstance *v = NULL;
    if (!g_launch_vm[0]) { KillTimer(hwnd, IDT_LAUNCH_VM); return; }
    /* Wait for start-up, and for a VM operation running on the worker thread. */
    if (!g_prereq_ok || g_bg_op || g_command_depth) {
        if (++g_launch_tries > 200) { KillTimer(hwnd, IDT_LAUNCH_VM); g_launch_vm[0] = 0; }
        return;
    }
    KillTimer(hwnd, IDT_LAUNCH_VM);
    for (i = 0; i < asb_vm_count(); i++) {
        VmInstance *c = asb_vm_instance(asb_vm_get(i));
        if (c && _wcsicmp(c->name, g_launch_vm) == 0) { idx = i; v = c; break; }
    }
    if (!v) {
        wchar_t msg[400];
        swprintf_s(msg, ARRAYSIZE(msg), L"There is no VM named \"%s\" anymore. "
                   L"Delete the shortcut or create it again.", g_launch_vm);
        g_launch_vm[0] = 0;
        ShowWindow(hwnd, SW_RESTORE);
        ui_alert(L"error", L"VM not found", msg);
        return;
    }
    g_launch_vm[0] = 0;
    if (v->running) {
        do_connect_vm(idx);
    } else if (v->starting || asb_vm_is_building(asb_vm_get(idx))) {
        ShowWindow(hwnd, SW_RESTORE);
        ui_alert(L"info", NULL, L"That VM is busy right now; try again in a moment.");
    } else {
        HRESULT hr = asb_vm_start(asb_vm_get(idx), -1, -1, NULL);
        if (FAILED(hr)) {
            ShowWindow(hwnd, SW_RESTORE);
            ui_show_alert(L"The VM could not be started. See the log in App Sandbox.");
        }
    }
    send_vm_list();
}

/* Shortcut file name: the VM name without characters Windows forbids. */
static void shortcut_file_name(const wchar_t *vm_name, wchar_t *out, size_t cap)
{
    size_t i, n = 0;
    for (i = 0; vm_name[i] && n + 1 < cap; i++) {
        wchar_t c = vm_name[i];
        out[n++] = (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    }
    out[n] = 0;
}

/* Creates "<VM>.lnk" on the desktop or in Start menu > App Sandbox.
   Returns S_OK and the path in out_path. */
static HRESULT create_vm_shortcut(const wchar_t *vm_name, BOOL start_menu,
                                  wchar_t *out_path, size_t out_cap, const VmInstance *vm_for_icon)
{
    PWSTR folder = NULL;
    wchar_t exe[MAX_PATH], dir[MAX_PATH], args[400], file[260], desc[400], *slash;
    IShellLinkW *link = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr;

    if (wcschr(vm_name, L'"')) return E_INVALIDARG;
    hr = SHGetKnownFolderPath(start_menu ? &FOLDERID_Programs : &FOLDERID_Desktop, 0, NULL, &folder);
    if (FAILED(hr)) return hr;
    if (start_menu) {
        swprintf_s(dir, MAX_PATH, L"%s\\App Sandbox", folder);
        CreateDirectoryW(dir, NULL);
    } else {
        wcscpy_s(dir, MAX_PATH, folder);
    }
    CoTaskMemFree(folder);

    shortcut_file_name(vm_name, file, ARRAYSIZE(file));
    swprintf_s(out_path, out_cap, L"%s\\%s.lnk", dir, file);

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (asb_running_packaged()) {
        /* MSIX: files in WindowsApps cannot be started directly; use the
           package's execution alias (%LOCALAPPDATA%\Microsoft\WindowsApps). */
        PWSTR local = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
            swprintf_s(exe, MAX_PATH, L"%s\\Microsoft\\WindowsApps\\AppSandbox.exe", local);
            CoTaskMemFree(local);
        }
    }
    swprintf_s(args, ARRAYSIZE(args), L"--vm \"%s\"", vm_name);
    swprintf_s(desc, ARRAYSIZE(desc), L"Start %s in App Sandbox", vm_name);

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr)) return hr;
    link->lpVtbl->SetPath(link, exe);
    link->lpVtbl->SetArguments(link, args);
    wcscpy_s(dir, MAX_PATH, exe);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    link->lpVtbl->SetWorkingDirectory(link, dir);
    link->lpVtbl->SetDescription(link, desc);
    {
        wchar_t icon[MAX_PATH];
        GetModuleFileNameW(NULL, icon, MAX_PATH);
        if (vm_for_icon) {
            wchar_t custom[MAX_PATH];
            vm_icon_file(vm_for_icon, L"icon.ico", custom, MAX_PATH);
            if (GetFileAttributesW(custom) != INVALID_FILE_ATTRIBUTES)
                wcscpy_s(icon, MAX_PATH, custom);
        }
        link->lpVtbl->SetIconLocation(link, icon, 0);
    }
    hr = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        hr = pf->lpVtbl->Save(pf, out_path, TRUE);
        pf->lpVtbl->Release(pf);
    }
    link->lpVtbl->Release(link);
    if (SUCCEEDED(hr))
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, out_path, NULL);
    return hr;
}

static void do_connect_vm(int idx)
{
    VmInstance *vm;
    reconcile_displays();
    if (idx < 0 || idx >= asb_vm_count()) return;
    vm = asb_vm_instance(asb_vm_get(idx));
    if (!vm || !vm->running) return;
    asb_vm_set_display_suppressed((AsbVm)vm, FALSE);
    /* Manual-install VMs: the Hyper-V basic console over RDP (no App Sandbox
       display driver in the guest). */
    if (_wcsicmp(vm->os_type, L"Other") == 0) {
        do_connect_rdp(idx);
        return;
    }
    /* Direct ISO->VHDX flow boots straight into the configured rootfs with
       IDD live from the start, so the RDP fallback window is no longer
       needed. do_connect_rdp() + vm_display.c stay on disk as reference
       for the RDP-over-named-pipe pattern but are no longer invoked.
       The g_displays[]/safe_destroy_rdp cleanup paths remain inert
       because nothing populates the slots. */
    do_connect_idd(idx);
}

BOOL ui_display_is_open(UINT64 id)
{
    int i;
    for (i = 0; i < ASB_MAX_VMS; ++i)
        if (g_display_vm_ids[i] == id && g_idd_displays[i])
            return vm_display_idd_is_open(g_idd_displays[i]);
    return FALSE;
}
BOOL ui_display_open(UINT64 id)
{
    VmInstance *vm;
    int i;
    reconcile_displays();
    vm = asb_find_vm_by_id(id);
    if (!vm) return FALSE;
    do_connect_vm(asb_vm_index((AsbVm)vm));
    for (i = 0; i < ASB_MAX_VMS; ++i)
        if (g_display_vm_ids[i] == id && g_idd_displays[i])
            return vm_display_idd_is_active(g_idd_displays[i]);
    return FALSE;
}
void ui_display_close(UINT64 id)
{
    int i;
    asb_vm_set_display_suppressed((AsbVm)asb_find_vm_by_id(id), TRUE);
    for (i = 0; i < ASB_MAX_VMS; ++i) {
        if (g_display_vm_ids[i] != id) continue;
        safe_destroy_rdp(i);
        safe_destroy_idd(i);
        g_display_vm_ids[i] = 0;
    }
    asb_events_changed();
}

/* ---- VM operations on the worker thread ---- */

static BOOL id_listed(const UINT64 *list, int count, UINT64 id)
{
    int i;
    for (i = 0; i < count; i++)
        if (list[i] == id) return TRUE;
    return FALSE;
}

static void id_add(UINT64 *list, int *count, UINT64 id)
{
    if (!id_listed(list, *count, id) && *count < ASB_MAX_VMS) list[(*count)++] = id;
}

static void bg_op_run(BgOp *op)
{
    int i;
    for (i = 0; i < op->count; i++) {
        VmInstance *v = asb_find_vm_by_id(op->ids[i]);
        if (!v) { op->results[i] = S_FALSE; continue; }
        switch (op->kind) {
        case BG_SHUTDOWN:
        case BG_SHUTDOWN_ALL: op->results[i] = asb_vm_shutdown((AsbVm)v); break;
        case BG_STOP:
        case BG_STOP_ALL:     op->results[i] = asb_vm_stop((AsbVm)v); break;
        case BG_DELETE:       op->results[i] = asb_vm_delete((AsbVm)v); break;
        }
    }
}

static DWORD WINAPI bg_op_thread(LPVOID param)
{
    BgOp *op = (BgOp *)param;
    int tries;
    bg_op_run(op);
    /* The UI thread takes op back with WM_BG_OP_DONE. When the window is
       gone, WM_DESTROY has waited for this thread and frees op. */
    for (tries = 0; tries < 50; tries++) {
        HWND hwnd = g_hwnd_main;
        if (!hwnd || PostMessageW(hwnd, WM_BG_OP_DONE, 0, (LPARAM)op)) break;
        Sleep(100);
    }
    return 0;
}

/* UI thread: report the results of a finished operation. */
static void bg_op_finish(BgOp *op)
{
    int i;
    if (g_bg_op == op) g_bg_op = NULL;
    if (g_bg_thread) {
        WaitForSingleObject(g_bg_thread, 5000);
        CloseHandle(g_bg_thread);
        g_bg_thread = NULL;
    }
    reconcile_displays();
    for (i = 0; i < op->count; i++) {
        HRESULT hr = op->results[i];
        VmInstance *v = asb_find_vm_by_id(op->ids[i]);
        wchar_t reason[300], text[512];
        if (hr == S_FALSE) continue;            /* not running any more, or gone */
        if (SUCCEEDED(hr)) {
            if (op->kind == BG_SHUTDOWN && v) {
                /* The guest accepted: close its windows while it shuts down. */
                int idx = asb_vm_index((AsbVm)v);
                safe_destroy_rdp(idx);
                safe_destroy_idd(idx);
            } else if (op->kind == BG_DELETE) {
                ui_log(L"VM \"%s\" deleted.", op->names[i]);
            }
            continue;
        }
        describe_hresult(hr, reason, ARRAYSIZE(reason));
        switch (op->kind) {
        case BG_SHUTDOWN:
        case BG_SHUTDOWN_ALL:
            _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"Could not be asked to shut down: %s", reason);
            if (op->kind == BG_SHUTDOWN_ALL) id_add(g_exit_refused, &g_exit_refused_count, op->ids[i]);
            break;
        case BG_STOP:
        case BG_STOP_ALL:
            _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE,
                         L"Force stop reported a problem: %s Details are in Activity.", reason);
            break;
        default:
            ui_log(L"VM deletion failed (0x%08X). Check that its disk storage is available and writable.", hr);
            _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, hr == E_ACCESSDENIED
                         ? L"Could not be deleted: %s"
                         : L"Could not be deleted: %s Check that its disk folder is available and writable; details are in Activity.",
                         reason);
            break;
        }
        if (v) {
            vm_note_set(op->ids[i], text);
        } else {
            wchar_t message[700];
            _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"\"%s\": %s", op->names[i], text);
            ui_notice(L"error", message);
        }
    }
    free(op);
    send_vm_list();
    ui_refresh_from_core();
    /* Let API requests that waited for this operation run now. */
    for (i = 0; i < g_deferred_api_count; i++)
        if (g_hwnd_main) PostMessageW(g_hwnd_main, WM_ASB_API_REQUEST, 0, g_deferred_api[i]);
    g_deferred_api_count = 0;
    /* Inside a dialog, an API request or a nested dispatch the exit timer
       continues on its next tick. */
    if (g_exit_waiting && g_hwnd_main && exit_may_run()) exit_tick(g_hwnd_main);
}

/* Run an operation for the listed VMs on the worker thread (inline when no
   thread can be created). FALSE when another one is still running. */
static BOOL bg_op_start(BgOpKind kind, const UINT64 *ids, int count)
{
    BgOp *op;
    HANDLE thread;
    int i;
    if (g_bg_op || count <= 0) return FALSE;
    op = (BgOp *)calloc(1, sizeof(*op));
    if (!op) return FALSE;
    op->kind = kind;
    op->count = count > ASB_MAX_VMS ? ASB_MAX_VMS : count;
    for (i = 0; i < op->count; i++) {
        VmInstance *v = asb_find_vm_by_id(ids[i]);
        op->ids[i] = ids[i];
        if (v) wcsncpy_s(op->names[i], ARRAYSIZE(op->names[i]), v->name, _TRUNCATE);
    }
    g_bg_op = op;
    send_vm_list();   /* cards show "Stopping" / "Deleting" at once */
    thread = CreateThread(NULL, 0, bg_op_thread, op, 0, NULL);
    if (!thread) {
        bg_op_run(op);
        bg_op_finish(op);
        return TRUE;
    }
    g_bg_thread = thread;
    return TRUE;
}

static BOOL bg_op_start_one(BgOpKind kind, UINT64 id)
{
    return bg_op_start(kind, &id, 1);
}

/* ---- Exit: never turn VMs off without asking ----
   Exiting the process turns every VM off (HCS terminates a compute system
   when its last handle closes). Tray Exit and closing the window therefore
   ask first, and "Shut down VMs and exit" asks each guest to shut down and
   exits once they have stopped. When Windows signs out, restarts or the
   package is updated, the same sequence runs while a shutdown block reason
   holds Windows back. */

static void count_vm_activity(int *running, int *busy)
{
    int i, count = asb_vm_count();
    *running = *busy = 0;
    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v) continue;
        if (v->running) (*running)++;
        if (v->starting || v->building_vhdx || v->copying_disk || v->updating_tools) (*busy)++;
    }
}

static void post_exit_progress(BOOL active, int remaining)
{
    wchar_t buf[160];
    swprintf_s(buf, ARRAYSIZE(buf),
               L"{\"type\":\"exitProgress\",\"active\":%s,\"remaining\":%d,\"session\":%s}",
               active ? L"true" : L"false", remaining, g_exit_for_session ? L"true" : L"false");
    webview2_post(buf);
}

/* The exit sequence starts VM operations, opens dialogs and finally
   destroys the window. It only runs from the outermost message dispatch:
   not inside a dialog or command (g_command_depth), a local API request,
   a display teardown that dispatches messages while it waits, or a nested
   main_wnd_proc call. Otherwise WM_DESTROY could run under a handler that
   still uses the state it frees, or the worker could act on a VM that the
   UI thread is changing. The 1 s timer retries. */
static BOOL exit_may_run(void)
{
    return !g_command_depth && !g_pump_depth && g_wndproc_depth <= 1 &&
           !asb_api_busy() && !g_destroying;
}

static void exit_finish(HWND hwnd)
{
    KillTimer(hwnd, IDT_EXIT_WAIT);
    g_exit_waiting = FALSE;
    post_exit_progress(FALSE, 0);
    DestroyWindow(hwnd);
}

static void exit_cancel(HWND hwnd)
{
    if (!g_exit_waiting) return;
    KillTimer(hwnd, IDT_EXIT_WAIT);
    g_exit_waiting = g_exit_for_session = g_exit_forced = FALSE;
    if (g_exit_block_reason) {
        ShutdownBlockReasonDestroy(hwnd);
        g_exit_block_reason = FALSE;
    }
    post_exit_progress(FALSE, 0);
    ui_log(L"Exit cancelled; App Sandbox keeps running.");
}

static void exit_begin(HWND hwnd, BOOL for_session)
{
    if (g_exit_waiting) {
        if (for_session) g_exit_for_session = TRUE;
        return;
    }
    g_exit_waiting = TRUE;
    g_exit_for_session = for_session;
    g_exit_forced = FALSE;
    g_exit_started = GetTickCount64();
    g_exit_asked_count = g_exit_refused_count = 0;
    ui_log(for_session
        ? L"Windows is signing out, restarting or updating App Sandbox: shutting the VMs down first."
        : L"Shutting the VMs down; App Sandbox exits when they have stopped.");
    SetTimer(hwnd, IDT_EXIT_WAIT, 1000, NULL);
    if (exit_may_run()) exit_tick(hwnd);   /* otherwise the first tick does it */
}

/* Some VMs did not stop in time, or could not be asked to. */
static void exit_timeout(HWND hwnd, const UINT64 *ids, int n)
{
    wchar_t names[1024] = L"", content[1500], instruction[96];
    TASKDIALOGCONFIG tdc;
    TASKDIALOG_BUTTON buttons[3];
    int i, btn = 0;
    HRESULT hr;

    /* Windows is ending the session: whatever still runs is turned off with
       the app now. */
    if (g_exit_for_session) { exit_finish(hwnd); return; }

    KillTimer(hwnd, IDT_EXIT_WAIT);
    for (i = 0; i < n; i++) {
        VmInstance *v = asb_find_vm_by_id(ids[i]);
        if (!v) continue;
        if (names[0]) wcsncat_s(names, ARRAYSIZE(names), L", ", _TRUNCATE);
        wcsncat_s(names, ARRAYSIZE(names), v->name, _TRUNCATE);
    }
    _snwprintf_s(content, ARRAYSIZE(content), _TRUNCATE,
        L"Still running: %s\n\nA VM may be waiting for an app inside it to close, or it has no "
        L"guest agent to receive the request. Force stop turns it off at once; unsaved work in "
        L"it is lost.", names);
    if (n == 1) wcscpy_s(instruction, ARRAYSIZE(instruction), L"A VM has not shut down");
    else swprintf_s(instruction, ARRAYSIZE(instruction), L"%d VMs have not shut down", n);

    buttons[0].nButtonID = 100; buttons[0].pszButtonText = L"Force stop and exit";
    buttons[1].nButtonID = 101; buttons[1].pszButtonText = L"Keep waiting";
    buttons[2].nButtonID = 102; buttons[2].pszButtonText = L"Don't exit";
    ZeroMemory(&tdc, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = IsWindowVisible(hwnd) ? hwnd : NULL;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.pszWindowTitle = L"App Sandbox";
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszMainInstruction = instruction;
    tdc.pszContent = content;
    tdc.cButtons = 3;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = 101;

    ++g_command_depth;
    hr = TaskDialogIndirect(&tdc, &btn, NULL, NULL);
    --g_command_depth;
    if (!g_exit_waiting) return;
    if (FAILED(hr) || btn == 102 || btn == IDCANCEL) { exit_cancel(hwnd); return; }

    SetTimer(hwnd, IDT_EXIT_WAIT, 1000, NULL);
    if (btn == 100) {
        UINT64 running[ASB_MAX_VMS];
        int count = asb_vm_count(), k = 0;
        for (i = 0; i < count; i++) {
            VmInstance *v = asb_vm_instance(asb_vm_get(i));
            if (v && v->running) running[k++] = v->unique_id;
        }
        g_exit_forced = TRUE;
        ui_log(L"Force stopping the VMs that did not shut down, then exiting.");
        if (!bg_op_start(BG_STOP_ALL, running, k)) exit_tick(hwnd);
    } else {
        /* Keep waiting: ask the VMs that refused again (their guest agent
           may have connected meanwhile) and restart the time limit. */
        int k = 0;
        for (i = 0; i < g_exit_asked_count; i++)
            if (!id_listed(g_exit_refused, g_exit_refused_count, g_exit_asked[i]))
                g_exit_asked[k++] = g_exit_asked[i];
        g_exit_asked_count = k;
        g_exit_refused_count = 0;
        g_exit_started = GetTickCount64();
    }
}

/* Once a second while exiting: ask VMs that run to shut down, exit when
   none runs, ask what to do when they take too long. */
static void exit_tick_run(HWND hwnd)
{
    UINT64 ask[ASB_MAX_VMS], active[ASB_MAX_VMS];
    int i, n_ask = 0, n_active = 0, waiting = 0, count = asb_vm_count();

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v || !(v->running || v->starting)) continue;
        active[n_active++] = v->unique_id;
        if (!v->running) { waiting++; continue; }   /* starting: asked once it runs */
        if (!id_listed(g_exit_asked, g_exit_asked_count, v->unique_id)) ask[n_ask++] = v->unique_id;
        else if (!id_listed(g_exit_refused, g_exit_refused_count, v->unique_id)) waiting++;
    }
    post_exit_progress(TRUE, n_active);
    if (g_bg_op) return;                             /* requests are still being sent */
    if (n_active == 0 || g_exit_forced) { exit_finish(hwnd); return; }
    if (n_ask > 0) {
        for (i = 0; i < n_ask; i++) id_add(g_exit_asked, &g_exit_asked_count, ask[i]);
        if (!bg_op_start(BG_SHUTDOWN_ALL, ask, n_ask))
            for (i = 0; i < n_ask; i++) id_add(g_exit_refused, &g_exit_refused_count, ask[i]);
        return;
    }
    if (waiting > 0 && GetTickCount64() - g_exit_started < EXIT_WAIT_LIMIT_MS) return;
    exit_timeout(hwnd, active, n_active);
}

static void exit_tick(HWND hwnd)
{
    static BOOL ticking;   /* an operation finished inline re-enters here */
    if (!g_exit_waiting || ticking) return;
    ticking = TRUE;
    exit_tick_run(hwnd);
    ticking = FALSE;
}

/* Tray Exit and closing the window: decide with the user what happens to
   running VMs and operations in progress. */
static void confirm_exit(HWND hwnd, BOOL from_close)
{
    enum { BTN_TRAY = 200, BTN_SHUTDOWN, BTN_EXIT_NOW };
    TASKDIALOGCONFIG tdc;
    TASKDIALOG_BUTTON buttons[3];
    wchar_t instruction[96], content[700];
    int running = 0, busy = 0, nbuttons = 0, btn = 0;
    BOOL checked = FALSE;
    HRESULT hr;

    if (g_exit_waiting) {                 /* already exiting: keep going in the tray */
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    count_vm_activity(&running, &busy);
    if (!running && !busy) { DestroyWindow(hwnd); return; }
    if (from_close && running && !busy && asb_get_suppress_tray_warn()) {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }

    if (running) {
        if (running == 1) wcscpy_s(instruction, ARRAYSIZE(instruction), L"A VM is still running");
        else swprintf_s(instruction, ARRAYSIZE(instruction), L"%d VMs are still running", running);
        wcscpy_s(content, ARRAYSIZE(content),
                 L"Exiting App Sandbox turns its VMs off. Shut them down first so the apps and "
                 L"files open in them are saved.");
    } else {
        wcscpy_s(instruction, ARRAYSIZE(instruction), L"A VM operation is still in progress");
        content[0] = 0;
    }
    if (busy) {
        wchar_t more[300];
        swprintf_s(more, ARRAYSIZE(more),
                   L"%s%d VM%s being created, started, imported, exported or updated. Exiting "
                   L"stops that work and can leave an incomplete VM behind.",
                   content[0] ? L"\n\n" : L"", busy, busy == 1 ? L" is" : L"s are");
        wcsncat_s(content, ARRAYSIZE(content), more, _TRUNCATE);
    }

    buttons[nbuttons].nButtonID = BTN_TRAY;
    buttons[nbuttons++].pszButtonText =
        L"Keep running in the tray\nThe window closes and the VMs keep running. Open App Sandbox again from the notification area.";
    if (running) {
        buttons[nbuttons].nButtonID = BTN_SHUTDOWN;
        buttons[nbuttons++].pszButtonText =
            L"Shut down VMs and exit\nEach VM is asked to shut down. App Sandbox exits when they have stopped.";
    } else {
        buttons[nbuttons].nButtonID = BTN_EXIT_NOW;
        buttons[nbuttons++].pszButtonText = L"Exit anyway\nStops the operation in progress.";
    }

    ZeroMemory(&tdc, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = IsWindowVisible(hwnd) ? hwnd : NULL;
    tdc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tdc.pszWindowTitle = L"App Sandbox";
    tdc.pszMainIcon = busy ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    tdc.pszMainInstruction = instruction;
    tdc.pszContent = content;
    tdc.cButtons = nbuttons;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = (from_close || busy) ? BTN_TRAY : BTN_SHUTDOWN;
    if (from_close && running && !busy)
        tdc.pszVerificationText = L"Always keep VMs running in the tray when I close the window";

    ++g_command_depth;
    hr = TaskDialogIndirect(&tdc, &btn, NULL, &checked);
    --g_command_depth;
    if (FAILED(hr)) return;

    if (btn == BTN_TRAY) {
        if (checked) {
            asb_set_suppress_tray_warn(TRUE);
            asb_save();
            send_host_info();
        }
        ShowWindow(hwnd, SW_HIDE);
    } else if (btn == BTN_SHUTDOWN) {
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        exit_begin(hwnd, FALSE);
    } else if (btn == BTN_EXIT_NOW) {
        DestroyWindow(hwnd);
    }
}

/* Tray "Force stop": the same warning the page shows. */
static BOOL confirm_force_stop(HWND hwnd, const wchar_t *name)
{
    TASKDIALOGCONFIG tdc;
    TASKDIALOG_BUTTON button;
    wchar_t content[600];
    int btn = 0;
    HRESULT hr;

    _snwprintf_s(content, ARRAYSIZE(content), _TRUNCATE,
        L"Force stop turns \"%s\" off at once, like pulling the plug. Unsaved work in it is lost "
        L"and its disk can be damaged. Use Shut down when you can.", name);
    button.nButtonID = IDOK;
    button.pszButtonText = L"Force stop";
    ZeroMemory(&tdc, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = IsWindowVisible(hwnd) ? hwnd : NULL;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tdc.pszWindowTitle = L"App Sandbox";
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszMainInstruction = L"Force stop this VM?";
    tdc.pszContent = content;
    tdc.cButtons = 1;
    tdc.pButtons = &button;
    tdc.nDefaultButton = IDCANCEL;
    ++g_command_depth;
    hr = TaskDialogIndirect(&tdc, &btn, NULL, NULL);
    --g_command_depth;
    return SUCCEEDED(hr) && btn == IDOK;
}

/* ---- Folders, clipboard, diagnostics and links (Settings, VM menu) ----
   Only paths App Sandbox owns are opened: its data folder, the default VM
   folder, its log, its own folder and a VM's folder from vms.cfg. The page
   names which one, never a path, and links come from a fixed list. */

static BOOL path_is_directory(const wchar_t *path)
{
    DWORD attrs = path && path[0] ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL path_is_file(const wchar_t *path)
{
    DWORD attrs = path && path[0] ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Show a folder in File Explorer. */
static BOOL explorer_open_folder(const wchar_t *folder)
{
    if (!path_is_directory(folder)) return FALSE;
    return (INT_PTR)ShellExecuteW(g_hwnd_main, L"open", folder, NULL, NULL, SW_SHOWNORMAL) > 32;
}

/* Show the folder holding path in File Explorer, with path selected. */
static BOOL explorer_select(const wchar_t *path)
{
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr;
    if (!path || !path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    pidl = ILCreateFromPathW(path);
    if (!pidl) return FALSE;
    hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

/* Open an https link through Explorer, so the browser does not run with
   App Sandbox's administrator rights. */
static void open_url(const wchar_t *url)
{
    wchar_t explorer[MAX_PATH], args[400];
    UINT n = GetWindowsDirectoryW(explorer, MAX_PATH);
    if (wcsncmp(url, L"https://", 8) != 0) return;
    if (n && n < MAX_PATH - 16) {
        wcscat_s(explorer, MAX_PATH, L"\\explorer.exe");
        swprintf_s(args, ARRAYSIZE(args), L"\"%s\"", url);
        if ((INT_PTR)ShellExecuteW(g_hwnd_main, L"open", explorer, args, NULL, SW_SHOWNORMAL) > 32)
            return;
    }
    ShellExecuteW(g_hwnd_main, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* Links of the About section, by name. */
static void open_project_link(const wchar_t *which)
{
    wchar_t url[300];
    if (wcscmp(which, L"project") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s", UI_PROJECT_REPO);
    else if (wcscmp(which, L"issues") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s/issues", UI_PROJECT_REPO);
    else if (wcscmp(which, L"releases") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s/releases", UI_PROJECT_REPO);
    else if (wcscmp(which, L"license") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s/blob/main/LICENSE", UI_PROJECT_REPO);
    else if (wcscmp(which, L"notices") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s/blob/main/THIRD-PARTY-NOTICES.md", UI_PROJECT_REPO);
    else if (wcscmp(which, L"upstream") == 0)
        swprintf_s(url, ARRAYSIZE(url), L"https://github.com/%s", UI_UPSTREAM_REPO);
    else
        return;
    open_url(url);
}

static BOOL clipboard_set_text(const wchar_t *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL mem;
    BOOL ok = FALSE;
    if (!OpenClipboard(g_hwnd_main)) return FALSE;
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *p = GlobalLock(mem);
        if (p) {
            memcpy(p, text, bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

/* A small UTF-8 text file shipped beside AppSandbox.exe (LICENSE,
   THIRD-PARTY-NOTICES.md), or NULL. The caller frees it. */
static wchar_t *read_text_beside_exe(const wchar_t *file_name)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD size, got = 0;
    char *data;
    wchar_t *text = NULL;

    app_directory(path, MAX_PATH);
    if (!path[0] || wcslen(path) + wcslen(file_name) + 2 >= MAX_PATH) return NULL;
    wcscat_s(path, MAX_PATH, L"\\");
    wcscat_s(path, MAX_PATH, file_name);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 512 * 1024) { CloseHandle(f); return NULL; }
    data = (char *)malloc(size);
    if (data && ReadFile(f, data, size, &got, NULL) && got == size) {
        const char *p = data;
        int len = (int)size, n;
        if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;
            len -= 3;
        }
        n = len > 0 ? MultiByteToWideChar(CP_UTF8, 0, p, len, NULL, 0) : 0;
        if (n > 0 && (text = (wchar_t *)malloc((n + 1) * sizeof(wchar_t))) != NULL) {
            MultiByteToWideChar(CP_UTF8, 0, p, len, text, n);
            text[n] = 0;
        }
    }
    free(data);
    CloseHandle(f);
    return text;
}

static void diag_append(wchar_t *buf, size_t cap, const wchar_t *fmt, ...)
{
    size_t len = wcslen(buf);
    va_list args;
    if (len + 1 >= cap) return;
    va_start(args, fmt);
    _vsnwprintf_s(buf + len, cap - len, _TRUNCATE, fmt, args);
    va_end(args);
}

/* Text for bug reports: versions, Windows, hardware, Hyper-V platform state
   and the VMs (no account names or passwords). */
static void build_diagnostics(wchar_t *buf, size_t cap)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    static const wchar_t *net_names[] = { L"none", L"NAT", L"external", L"internal" };
    RTL_OSVERSIONINFOW os;
    RtlGetVersionFn get_version;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    GpuList *gpus = asb_gpu_list();
    wchar_t display_version[64] = L"", edition[64] = L"", sysdir[MAX_PATH], dll[MAX_PATH], path[MAX_PATH];
    const wchar_t *arch;
    DWORD ubr = 0, size;
    int i, count = asb_vm_count();

    buf[0] = 0;
    diag_append(buf, cap, L"App Sandbox %s (%s)\r\n", app_version(),
                asb_running_packaged() ? L"Microsoft Store package" : L"zip");

    ZeroMemory(&os, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    get_version = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (get_version) get_version(&os);
    size = sizeof(ubr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                     RRF_RT_REG_DWORD, NULL, &ubr, &size) != ERROR_SUCCESS)
        ubr = 0;
    size = sizeof(display_version);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion",
                     RRF_RT_REG_SZ, NULL, display_version, &size) != ERROR_SUCCESS)
        display_version[0] = 0;
    size = sizeof(edition);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"EditionID",
                     RRF_RT_REG_SZ, NULL, edition, &size) != ERROR_SUCCESS)
        edition[0] = 0;
    diag_append(buf, cap, L"Windows %lu.%lu build %lu.%lu %s %s\r\n",
                os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, ubr, edition, display_version);

    GetNativeSystemInfo(&si);
    arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64"
         : si.wProcessorArchitecture == 12 /* PROCESSOR_ARCHITECTURE_ARM64 */ ? L"ARM64" : L"other";
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    diag_append(buf, cap, L"Host: %s, %lu logical processors, %llu MB memory\r\n",
                arch, si.dwNumberOfProcessors, ms.ullTotalPhys / (1024ULL * 1024ULL));
    if (gpus && gpus->count > 0) {
        for (i = 0; i < gpus->count; i++)
            diag_append(buf, cap, L"GPU: %s (%s)\r\n", gpus->gpus[i].name, gpus->gpus[i].location);
    } else {
        diag_append(buf, cap, L"GPU: none found for GPU partitioning\r\n");
    }

    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) wcscpy_s(sysdir, MAX_PATH, L"C:\\Windows\\System32");
    swprintf_s(dll, MAX_PATH, L"%s\\computecore.dll", sysdir);
    diag_append(buf, cap, L"Host Compute Service (computecore.dll): %s\r\n",
                !path_is_file(dll) ? L"missing" : GetModuleHandleW(L"computecore.dll") ? L"loaded" : L"present, not loaded");
    swprintf_s(dll, MAX_PATH, L"%s\\computenetwork.dll", sysdir);
    diag_append(buf, cap, L"Host Compute Network (computenetwork.dll): %s\r\n",
                !path_is_file(dll) ? L"missing" : GetModuleHandleW(L"computenetwork.dll") ? L"loaded" : L"present, not loaded");
    diag_append(buf, cap, L"Virtual Machine Platform: %s\r\n",
                g_prereq_ok ? L"on" : g_prereq_reboot_pending ? L"turned on, restart pending"
                : g_core_starting ? L"being checked" : L"off");

    diag_append(buf, cap, L"VMs: %d\r\n", count);
    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        wchar_t state[64];
        if (!v) continue;
        if (v->copying_disk) swprintf_s(state, ARRAYSIZE(state), L"copying disk %ld%%", (long)v->copy_progress);
        else if (v->updating_tools) wcscpy_s(state, ARRAYSIZE(state), L"updating components");
        else if (v->starting) wcscpy_s(state, ARRAYSIZE(state), L"starting");
        else if (v->building_vhdx) swprintf_s(state, ARRAYSIZE(state), L"building disk %d%%", v->vhdx_progress);
        else if (v->running) wcscpy_s(state, ARRAYSIZE(state), v->shutdown_requested ? L"shutting down" : L"running");
        else wcscpy_s(state, ARRAYSIZE(state), L"stopped");
        diag_append(buf, cap, L"- %s: %s%s, %s, %lu MB, %lu vCPU, GPU %s, network %s, agent %s%s\r\n",
                    v->name, v->os_type, v->is_template ? L" template" : L"", state,
                    v->ram_mb, v->cpu_cores,
                    v->gpu_mode ? (v->gpu_name[0] ? v->gpu_name : L"default") : L"none",
                    net_names[v->network_mode & 3],
                    v->agent_online ? L"connected" : L"not connected",
                    v->install_complete ? L"" : L", setup not finished");
    }

    log_file_path(path, MAX_PATH);
    diag_append(buf, cap, L"Log: %s\r\n", path);
    asb_program_data_dir(path, MAX_PATH);
    diag_append(buf, cap, L"Data folder: %s\r\n", path);
    app_directory(path, MAX_PATH);
    diag_append(buf, cap, L"App folder: %s\r\n", path);
}

/* ---- System tray ---- */

static void tray_add(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wcscpy_s(g_nid.szTip, 128, L"App Sandbox");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

/* ---- Themed tray menu ----
 *
 * HMENU is retained for layout + input handling, but every item is MF_OWNERDRAW
 * so we can paint it in the same palette as the WebView2 front end
 * (see web/style.css). A WH_CALLWNDPROC hook paints the outer frame dark
 * via DWMWA_BORDER_COLOR (no-op on Win10 / pre-22H2). */

#define TRAY_BG_COLOR       RGB(0x1e, 0x1e, 0x1e)  /* --bg */
#define TRAY_SEL_COLOR      RGB(0x2a, 0x5a, 0x8a)  /* primary btn bg */
#define TRAY_TEXT_COLOR     RGB(0xe6, 0xe6, 0xe6)  /* --text */
#define TRAY_TEXT_DIM_COLOR RGB(0x88, 0x88, 0x88)  /* --text-dim */
#define TRAY_SEP_COLOR      RGB(0x3e, 0x3e, 0x3e)  /* --ctrl-border */

typedef struct {
    wchar_t label[256];
    BOOL    is_separator;
    BOOL    is_header;    /* dim label used as a section heading */
    BOOL    is_indented;  /* indent under a header */
} TrayDrawItem;

static TrayDrawItem g_tray_items[64];
static int          g_tray_item_count;
static HBRUSH       g_tray_bg_brush;

static HFONT tray_get_font(void)
{
    static HFONT font = NULL;
    if (!font) {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            font = CreateFontIndirectW(&ncm.lfMenuFont);
        if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return font;
}

static void tray_append(HMENU menu, UINT id, const wchar_t *label,
                        BOOL is_separator, BOOL is_header, BOOL is_indented)
{
    TrayDrawItem *it;
    UINT flags;
    if (g_tray_item_count >= 64) return;

    it = &g_tray_items[g_tray_item_count];
    ZeroMemory(it, sizeof(*it));
    if (label) wcscpy_s(it->label, 256, label);
    it->is_separator = is_separator;
    it->is_header    = is_header;
    it->is_indented  = is_indented;

    flags = MF_OWNERDRAW | (is_separator ? MF_SEPARATOR : 0);
    AppendMenuW(menu, flags, id, (LPCWSTR)(UINT_PTR)g_tray_item_count);
    g_tray_item_count++;
}

static void tray_handle_measure(MEASUREITEMSTRUCT *mis)
{
    TrayDrawItem *it;
    HDC hdc;
    HFONT old;
    SIZE sz;

    if (!mis || mis->CtlType != ODT_MENU) return;
    if ((int)mis->itemData >= g_tray_item_count) return;

    it = &g_tray_items[mis->itemData];
    if (it->is_separator) {
        mis->itemHeight = 7;
        mis->itemWidth  = 0;
        return;
    }

    hdc = GetDC(NULL);
    old = (HFONT)SelectObject(hdc, tray_get_font());
    GetTextExtentPoint32W(hdc, it->label, (int)wcslen(it->label), &sz);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    mis->itemHeight = sz.cy + (it->is_header ? 6 : 10);
    mis->itemWidth  = sz.cx + (it->is_indented ? 40 : 28);
}

static void tray_handle_draw(DRAWITEMSTRUCT *dis)
{
    TrayDrawItem *it;
    RECT rc;
    BOOL selected, grayed;
    COLORREF bg, fg;
    HBRUSH bg_brush;
    HFONT old_font;
    int indent;

    if (!dis || dis->CtlType != ODT_MENU) return;
    if ((int)dis->itemData >= g_tray_item_count) return;

    it = &g_tray_items[dis->itemData];
    rc = dis->rcItem;

    if (it->is_separator) {
        HPEN pen, old_pen;
        int y;
        bg_brush = CreateSolidBrush(TRAY_BG_COLOR);
        FillRect(dis->hDC, &rc, bg_brush);
        DeleteObject(bg_brush);
        pen = CreatePen(PS_SOLID, 1, TRAY_SEP_COLOR);
        old_pen = (HPEN)SelectObject(dis->hDC, pen);
        y = (rc.top + rc.bottom) / 2;
        MoveToEx(dis->hDC, rc.left + 14, y, NULL);
        LineTo(dis->hDC, rc.right - 14, y);
        SelectObject(dis->hDC, old_pen);
        DeleteObject(pen);
        return;
    }

    selected = (dis->itemState & ODS_SELECTED) && !(dis->itemState & ODS_GRAYED);
    grayed   = (dis->itemState & ODS_GRAYED) != 0;

    bg = selected ? TRAY_SEL_COLOR : TRAY_BG_COLOR;
    fg = (grayed || it->is_header) ? TRAY_TEXT_DIM_COLOR : TRAY_TEXT_COLOR;

    bg_brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rc, bg_brush);
    DeleteObject(bg_brush);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    old_font = (HFONT)SelectObject(dis->hDC, tray_get_font());

    indent = it->is_indented ? 28 : 14;
    rc.left += indent;
    rc.right -= 14;
    DrawTextW(dis->hDC, it->label, -1, &rc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    SelectObject(dis->hDC, old_font);
}

/* WH_CALLWNDPROC hook: catches messages sent to the popup menu window
   (class "#32768") as TrackPopupMenu brings it up, and paints its border
   the same color as the menu background so it visually disappears. */
static LRESULT CALLBACK tray_menu_hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        CWPSTRUCT *cwp = (CWPSTRUCT *)lp;
        wchar_t cls[16];
        if (GetClassNameW(cwp->hwnd, cls, 16) > 0 && wcscmp(cls, L"#32768") == 0) {
            COLORREF border = TRAY_BG_COLOR;
            DwmSetWindowAttribute(cwp->hwnd, DWMWA_BORDER_COLOR,
                                  &border, sizeof(border));
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int i, cmd, count = asb_vm_count();
    UINT64 vm_ids[ASB_MAX_VMS] = {0};
    HHOOK hook;
    MENUINFO mi;

    if (!g_tray_bg_brush) g_tray_bg_brush = CreateSolidBrush(TRAY_BG_COLOR);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize  = sizeof(mi);
    mi.fMask   = MIM_BACKGROUND;
    mi.hbrBack = g_tray_bg_brush;
    SetMenuInfo(menu, &mi);

    g_tray_item_count = 0;

    tray_append(menu, TRAY_CMD_SHOW, L"Show App Sandbox", FALSE, FALSE, FALSE);
    tray_append(menu, 0,             NULL,                TRUE,  FALSE, FALSE);

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v || !v->running) continue;
        vm_ids[i] = v->unique_id;
        tray_append(menu, 0, v->name, FALSE, TRUE, FALSE);
        tray_append(menu, TRAY_CMD_CONNECT_BASE + i,  L"\U0001F4FA  Connect",    FALSE, FALSE, TRUE);
        tray_append(menu, TRAY_CMD_SHUTDOWN_BASE + i, L"\u23FB  Shut down",      FALSE, FALSE, TRUE);
        tray_append(menu, TRAY_CMD_STOP_BASE + i,     L"\u2715  Force stop\u2026", FALSE, FALSE, TRUE);
        tray_append(menu, 0, NULL, TRUE, FALSE, FALSE);
    }

    tray_append(menu, TRAY_CMD_EXIT, L"Exit", FALSE, FALSE, FALSE);

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    hook = SetWindowsHookExW(WH_CALLWNDPROC, tray_menu_hook_proc,
                             NULL, GetCurrentThreadId());
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    if (hook) UnhookWindowsHookEx(hook);
    DestroyMenu(menu);

    if (cmd == TRAY_CMD_SHOW) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    } else if (cmd == TRAY_CMD_EXIT) {
        confirm_exit(hwnd, FALSE);
    } else if (cmd >= TRAY_CMD_STOP_BASE) {
        int slot = cmd - TRAY_CMD_STOP_BASE;
        VmInstance *vm = slot < ASB_MAX_VMS ? asb_find_vm_by_id(vm_ids[slot]) : NULL;
        wchar_t name[256];
        int vi;
        if (!vm) return;
        wcscpy_s(name, ARRAYSIZE(name), vm->name);
        if (!confirm_force_stop(hwnd, name)) return;
        /* Resolve again: the VM may have changed while the question was open. */
        vm = asb_find_vm_by_id(vm_ids[slot]);
        vi = vm ? asb_vm_index((AsbVm)vm) : -1;
        if (vi < 0 || g_bg_op) return;
        reconcile_displays();
        safe_destroy_rdp(vi);
        safe_destroy_idd(vi);
        bg_op_start_one(BG_STOP, vm->unique_id);
    } else if (cmd >= TRAY_CMD_SHUTDOWN_BASE) {
        int slot = cmd - TRAY_CMD_SHUTDOWN_BASE;
        VmInstance *vm = slot < ASB_MAX_VMS ? asb_find_vm_by_id(vm_ids[slot]) : NULL;
        if (!vm || g_bg_op) return;
        bg_op_start_one(BG_SHUTDOWN, vm->unique_id);
    } else if (cmd >= TRAY_CMD_CONNECT_BASE) {
        int slot = cmd - TRAY_CMD_CONNECT_BASE;
        VmInstance *vm = slot < ASB_MAX_VMS ? asb_find_vm_by_id(vm_ids[slot]) : NULL;
        if (vm) do_connect_vm(asb_vm_index((AsbVm)vm));
    }
}

/* ---- Prerequisite feature enable (background thread) ---- */

#define WM_PREREQ_PROGRESS (WM_APP + 18)

static void prereq_progress_cb(float pct, void *user_data)
{
    (void)user_data;
    /* Send integer percentage via PostMessage (truncate to int) */
    PostMessageW(g_hwnd_main, WM_PREREQ_PROGRESS, (WPARAM)(int)(pct + 0.5f), 0);
}

static DWORD WINAPI enable_feature_thread(LPVOID param)
{
    BOOL reboot_required = FALSE;
    BOOL ok;
    (void)param;

    ok = prereq_enable_feature(L"VirtualMachinePlatform", &reboot_required,
                                prereq_progress_cb, NULL);

    /* Pack result: WPARAM = success, LPARAM = reboot_required */
    PostMessageW(g_hwnd_main, WM_PREREQ_DONE, (WPARAM)ok, (LPARAM)reboot_required);
    return 0;
}

/* ---- WebView2 message dispatch ---- */

static BOOL json_vm_index(const wchar_t *json, int *idx)
{
    wchar_t id[ASB_VM_ID_TEXT_SIZE];
    if (!json_get_string(json, L"vmId", id, ARRAYSIZE(id))) return FALSE;
    *idx = asb_vm_index((AsbVm)asb_find_vm_by_id(asb_vm_id_parse_w(id)));
    return *idx >= 0;
}

static DWORD WINAPI core_start_thread(LPVOID param)
{
    BOOL ok;
    (void)param;
    ok = prereq_check_all();
    if (ok) asb_init();
    if (!g_hwnd_main || !PostMessageW(g_hwnd_main, WM_CORE_READY, ok ? 1 : 0, 0))
        InterlockedExchange(&g_core_starting, 0);
    return 0;
}

/* Start the core in the background (once). The UI shows a loading state
   until WM_CORE_READY. */
static void start_core_async(void)
{
    HANDLE t;
    if (g_prereq_ok || InterlockedCompareExchange(&g_core_starting, 1, 0) != 0) return;
    webview2_post(L"{\"type\":\"coreLoading\"}");
    t = CreateThread(NULL, 0, core_start_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else core_start_thread(NULL);   /* no thread: do it inline */
}

/* Answer to createVm: the dialog stays open until the VM is accepted and
   shows the reason when it is not. */
static void post_create_result(BOOL ok, const wchar_t *message)
{
    post_text_message(L"createResult", ok ? L"ok" : L"error", NULL, message ? message : L"");
}

/* A command that cannot run now: say why instead of dropping it. */
static void reply_rejected(const wchar_t *action, const wchar_t *message)
{
    if (wcscmp(action, L"createVm") == 0) post_create_result(FALSE, message);
    else ui_notice(L"info", message);
}

/* Commands that only read state or open something App Sandbox owns. They
   also run while the core starts, while a VM operation runs on the worker
   thread, or inside another command's dialog. */
static BOOL action_is_passive(const wchar_t *action)
{
    static const wchar_t *const passive[] = {
        L"uiReady", L"getState", L"setMinSize", L"selectVm", L"getVmIcon", L"getDiskSpace",
        L"openFolder", L"openVmFolder", L"copyVmPath", L"copyDiagnostics", L"openLink",
        L"showDocument", L"dismissVmError", L"cancelExit"
    };
    int i;
    for (i = 0; i < (int)ARRAYSIZE(passive); i++)
        if (wcscmp(action, passive[i]) == 0) return TRUE;
    return FALSE;
}

/* GetOpenFileName may change the current directory, and appsandbox.log is
   written relative to it: keep it where it was. */
static BOOL open_file_dialog(OPENFILENAMEW *ofn)
{
    wchar_t cwd[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
    BOOL ok;
    ofn->Flags |= OFN_NOCHANGEDIR;
    ok = GetOpenFileNameW(ofn);
    if (n && n < MAX_PATH) SetCurrentDirectoryW(cwd);
    return ok;
}

static void notify_snapshot_failure(const wchar_t *what, HRESULT hr)
{
    wchar_t reason[300], message[500];
    if (SUCCEEDED(hr)) return;
    describe_hresult(hr, reason, ARRAYSIZE(reason));
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"%s: %s Details are in Activity.", what, reason);
    ui_notice(L"error", message);
}

/* Smallest window size for the page's minimum, at the window's DPI, never
   larger than the monitor's work area. */
static SIZE min_window_size(HWND hwnd)
{
    SIZE size = { 0, 0 };
    MONITORINFO mi;
    RECT rc;
    UINT dpi;
    if (!hwnd || g_min_css_width <= 0 || g_min_css_height <= 0) return size;
    dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    rc.left = rc.top = 0;
    rc.right = MulDiv(g_min_css_width, (int)dpi, 96);
    rc.bottom = MulDiv(g_min_css_height, (int)dpi, 96);
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (size.cx > mi.rcWork.right - mi.rcWork.left) size.cx = mi.rcWork.right - mi.rcWork.left;
        if (size.cy > mi.rcWork.bottom - mi.rcWork.top) size.cy = mi.rcWork.bottom - mi.rcWork.top;
    }
    return size;
}

/* Run a VM operation on the worker thread; say so when it cannot start. */
static void start_vm_op(BgOpKind kind, UINT64 id)
{
    if (!bg_op_start_one(kind, id))
        ui_notice(L"info", L"Another VM operation is still running. Try again in a moment.");
}

static void handle_webview2_message(const wchar_t *json)
{
    wchar_t action[64] = { 0 };

    if (!json_get_string(json, L"action", action, 64))
        return;

    if (wcscmp(action, L"uiReady") == 0) {
        /* JS has loaded and registered its message listener; deliver any
         * messages queued before WebView2 was ready (e.g. early ui_log lines). */
        webview2_flush_queue();
        if (g_prereq_ok) {            /* page reloaded after start-up */
            send_full_state();   /* an exit in progress reports again with its next tick */
            return;
        }
        if (g_core_starting) {        /* page reloaded while the core starts */
            webview2_post(L"{\"type\":\"coreLoading\"}");
            return;
        }
        start_core_async();
    } else if (wcscmp(action, L"createVm") == 0) {
        AsbVmConfig cfg;
        wchar_t name_buf[256] = {0}, os_buf[32] = {0}, img_buf[MAX_PATH] = {0};
        wchar_t tpl_buf[256] = {0}, user_buf[128] = {0}, pass_buf[256] = {0};
        wchar_t adapter_buf[256] = {0}, disk_buf[MAX_PATH + 1] = {0};
        wchar_t gpu_id[512] = {0}, captured[1024];
        const wchar_t *error = NULL;
        HRESULT hr;
        int val;
        BOOL is_tpl = FALSE, manual_os;

        if (!g_prereq_ok) {
            webview2_post(g_prereq_reboot_pending
                ? L"{\"type\":\"prereqReboot\"}"
                : L"{\"type\":\"prereqRequired\"}");
            post_create_result(FALSE, g_prereq_reboot_pending
                ? L"Restart Windows to finish turning on Virtual Machine Platform, then create the VM."
                : L"Turn on Virtual Machine Platform first; App Sandbox needs it to run VMs.");
            return;
        }
        if (g_exit_waiting) {
            post_create_result(FALSE, L"App Sandbox is shutting its VMs down to exit. Cancel that first.");
            return;
        }

        json_get_string(json, L"name", name_buf, 256);
        json_get_string(json, L"osType", os_buf, 32);
        json_get_string(json, L"imagePath", img_buf, MAX_PATH);
        json_get_string(json, L"templateName", tpl_buf, 256);
        /* Manual install ("Other"): no account is created. */
        manual_os = _wcsicmp(os_buf, L"Other") == 0;
        if (!manual_os && !json_get_string(json, L"adminUser", user_buf, ARRAYSIZE(user_buf))) {
            error = !json_has_key(json, L"adminUser")
                ? L"Username is required."
                : user_buf[0]
                    ? (_wcsicmp(os_buf, L"Linux") == 0 && !tpl_buf[0]
                        ? L"Username cannot exceed 32 characters (Linux limit)."
                        : L"Username cannot exceed 20 characters.")
                    : L"adminUser must be a valid JSON string without NUL characters.";
        } else if (!manual_os && !json_get_string(json, L"adminPass", pass_buf, ARRAYSIZE(pass_buf))) {
            error = !json_has_key(json, L"adminPass")
                ? L"Password is required."
                : pass_buf[0]
                    ? (_wcsicmp(os_buf, L"Linux") == 0 && !tpl_buf[0]
                        ? L"Password is too long (max 255 bytes)."
                        : L"Password is too long (max 127 characters for Windows).")
                    : L"adminPass must be a valid JSON string without NUL characters.";
        } else if (!json_get_string(json, L"gpuId", gpu_id, ARRAYSIZE(gpu_id)) &&
                   json_has_key(json, L"gpuId")) {
            error = L"GPU selection is invalid or too long.";
        } else if (!json_get_string(json, L"diskDirectory", disk_buf, MAX_PATH + 1) &&
                   json_has_key(json, L"diskDirectory")) {
            error = L"Disk folder is invalid or too long.";
        }
        if (error) {
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            post_create_result(FALSE, error);
            return;
        }
        json_get_string(json, L"netAdapter", adapter_buf, 256);
        json_get_bool(json, L"isTemplate", &is_tpl);

        ZeroMemory(&cfg, sizeof(cfg));
        cfg.name = name_buf;
        cfg.os_type = os_buf;
        cfg.image_path = img_buf;
        cfg.template_name = tpl_buf;
        cfg.username = user_buf;
        cfg.password = pass_buf;
        cfg.net_adapter = adapter_buf;
        cfg.is_template = is_tpl;
        cfg.disk_directory = disk_buf;
        cfg.gpu_id = gpu_id;

        if (json_get_int(json, L"hddGb", &val)) cfg.hdd_gb = (DWORD)val;
        if (json_get_int(json, L"ramMb", &val)) cfg.ram_mb = (DWORD)val;
        if (json_get_int(json, L"cpuCores", &val)) cfg.cpu_cores = (DWORD)val;
        if (json_get_int(json, L"gpuMode", &val)) cfg.gpu_mode = val;
        if (json_get_int(json, L"networkMode", &val)) cfg.network_mode = val;
        json_get_bool(json, L"testMode", &cfg.test_mode);
        json_get_bool(json, L"sshEnabled", &cfg.ssh_enabled);
        json_get_bool(json, L"sshDeployKey", &cfg.ssh_deploy_key);

        error = asb_validate_gpu_selection(cfg.gpu_mode, cfg.gpu_id);
        if (error) {
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            post_create_result(FALSE, error);
            return;
        }
        g_last_error_line[0] = 0;
        alert_capture_begin(captured, ARRAYSIZE(captured));
        hr = asb_vm_create(&cfg);
        alert_capture_end();
        SecureZeroMemory(pass_buf, sizeof(pass_buf));
        if (FAILED(hr)) {
            wchar_t reason[700], message[1400];
            if (!captured[0]) {
                recent_error_reason(reason, ARRAYSIZE(reason));
                if (!reason[0]) {
                    wchar_t why[300];
                    describe_hresult(hr, why, ARRAYSIZE(why));
                    _snwprintf_s(reason, ARRAYSIZE(reason), _TRUNCATE, L" Reason: %s", why);
                }
                _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                             L"The VM could not be created.%s Details are in Activity.", reason);
            }
            post_create_result(FALSE, captured[0] ? captured : message);
            send_vm_list();
            return;
        }
        post_create_result(TRUE, NULL);
        /* Created, but something went wrong afterwards (for example it could not start). */
        if (captured[0]) ui_alert(L"error", NULL, captured);
        if (manual_os) {
            int k;
            for (k = 0; k < asb_vm_count(); k++) {
                VmInstance *cv = asb_vm_instance(asb_vm_get(k));
                if (cv && _wcsicmp(cv->name, name_buf) == 0) { open_console_when_running(cv->unique_id); break; }
            }
        }
        send_vm_list();
    } else if (wcscmp(action, L"startVm") == 0) {
        int idx, si = -1, bi = -1;
        wchar_t bname[128] = {0};
        if (!g_prereq_ok) {
            webview2_post(g_prereq_reboot_pending
                ? L"{\"type\":\"prereqReboot\"}"
                : L"{\"type\":\"prereqRequired\"}");
            return;
        }
        if (g_exit_waiting) {
            ui_notice(L"info", L"App Sandbox is shutting its VMs down to exit. Cancel that first to start a VM.");
            return;
        }
        if (json_vm_index(json, &idx)) {
            VmInstance *sv = asb_vm_instance(asb_vm_get(idx));
            wchar_t captured[1024];
            HRESULT hr;
            json_get_int(json, L"snapIndex", &si);
            json_get_int(json, L"branchIndex", &bi);
            json_get_string(json, L"branchName", bname, 128);
            if (sv) vm_note_clear(sv->unique_id);
            alert_capture_begin(captured, ARRAYSIZE(captured));
            hr = asb_vm_start(asb_vm_get(idx), si, bi, bname);
            alert_capture_end();
            if (FAILED(hr)) {
                wchar_t reason[300], text[1400];
                if (!captured[0]) describe_hresult(hr, reason, ARRAYSIZE(reason));
                _snwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, L"Could not start: %s%s",
                             captured[0] ? captured : reason,
                             captured[0] ? L"" : L" Details are in Activity.");
                if (sv) vm_note_set(sv->unique_id, text);
            } else {
                if (captured[0]) ui_alert(L"error", NULL, captured);
                if (sv && _wcsicmp(sv->os_type, L"Other") == 0)
                    open_console_when_running(sv->unique_id);   /* no display driver to auto-open */
            }
            send_vm_list();
        }
    } else if (wcscmp(action, L"shutdownVm") == 0) {
        /* The VM's windows close once the guest has accepted the request. */
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            vm_note_clear(v->unique_id);
            start_vm_op(BG_SHUTDOWN, v->unique_id);
        }
    } else if (wcscmp(action, L"stopVm") == 0) {
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
            vm_note_clear(v->unique_id);
            start_vm_op(BG_STOP, v->unique_id);
        }
    } else if (wcscmp(action, L"connectIddVm") == 0) {
        int idx; if (json_vm_index(json, &idx)) do_connect_vm(idx);
    } else if (wcscmp(action, L"sshConnect") == 0) {
        int idx;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count()) {
            VmInstance *inst = asb_vm_instance(asb_vm_get(idx));
            if (inst && inst->ssh_enabled && inst->ssh_port) {
                wchar_t cmd[1024], keyopt[700] = L"";
                STARTUPINFOW si_;
                PROCESS_INFORMATION pi_;
                ZeroMemory(&si_, sizeof(si_));
                si_.cb = sizeof(si_);
                ZeroMemory(&pi_, sizeof(pi_));
                /* If this VM had the AppSandbox key deployed, use it (-i) so the
                   terminal logs in with key auth instead of a password prompt.
                   IdentitiesOnly avoids offering the user's other keys, and -- since
                   these are ephemeral loopback VMs -- StrictHostKeyChecking=no +
                   a throwaway known_hosts skips the host-key fingerprint prompt and
                   keeps it out of the user's real known_hosts. Password logins (no
                   key) keep the normal fingerprint prompt. */
                if (inst->ssh_deploy_key) {
                    wchar_t base[MAX_PATH], tmp[MAX_PATH];
                    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
                        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
                    if (!GetTempPathW(MAX_PATH, tmp))
                        wcscpy_s(tmp, MAX_PATH, L"C:\\Windows\\Temp\\");
                    _snwprintf_s(keyopt, 700, _TRUNCATE,
                        L"-i \"%s\\AppSandbox\\ssh\\id_appsandbox\" -o IdentitiesOnly=yes "
                        L"-o StrictHostKeyChecking=no -o \"UserKnownHostsFile=%sappsandbox_known_hosts\" ",
                        base, tmp);
                }
                if (inst->admin_user[0])
                    _snwprintf_s(cmd, 1024, _TRUNCATE,
                        L"cmd.exe /v:off /k ssh %s-p %lu -l \"%s\" localhost",
                        keyopt, inst->ssh_port, inst->admin_user);
                else
                    _snwprintf_s(cmd, 1024, _TRUNCATE,
                        L"cmd.exe /k ssh %s-p %lu localhost",
                        keyopt, inst->ssh_port);
                ui_log(L"SSH: %s", cmd);
                if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                                    CREATE_NEW_CONSOLE, NULL, NULL, &si_, &pi_)) {
                    CloseHandle(pi_.hProcess);
                    CloseHandle(pi_.hThread);
                }
            }
        }
    } else if (wcscmp(action, L"deleteVm") == 0) {
        /* Runs on the worker thread; a running VM is turned off first. */
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            ui_log(L"Deleting VM \"%s\"...", v->name);
            vm_note_clear(v->unique_id);
            ui_display_close(v->unique_id);
            start_vm_op(BG_DELETE, v->unique_id);
        }
    } else if (wcscmp(action, L"deleteTemplate") == 0) {
        wchar_t tpl_name[256] = { 0 };
        json_get_string(json, L"name", tpl_name, 256);
        if (tpl_name[0] != L'\0') {
            HRESULT hr = asb_template_delete(tpl_name);
            if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
                ui_alert(L"info", L"Template in use", L"This template is still required by an existing VM or an active disk copy.");
            } else if (FAILED(hr)) {
                ui_log(L"Template deletion failed (0x%08X). Check that its disk storage is available and writable.", hr);
                ui_show_alert(L"Template deletion failed. Check that its disk storage is available and writable.");
            }
            send_templates();
        }
    } else if (wcscmp(action, L"editVm") == 0) {
        int idx;
        wchar_t field[64], value[256];
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count() &&
            json_get_string(json, L"field", field, 64) &&
            json_get_string(json, L"value", value, 256)) {
            AsbVm vm = asb_vm_get(idx);
            if (asb_vm_is_running(vm) || asb_vm_is_building(vm)) {
                ui_alert(L"info", L"Shut the VM down first", L"VM settings can only be changed when the VM is stopped and its disk build has finished.");
            } else {
                HRESULT hr = E_INVALIDARG;
                const wchar_t *error = NULL;
                if (wcscmp(field, L"name") == 0) hr = asb_vm_set_name(vm, value);
                else if (wcscmp(field, L"ramMb") == 0) hr = asb_vm_set_ram(vm, (DWORD)_wtoi(value));
                else if (wcscmp(field, L"cpuCores") == 0) hr = asb_vm_set_cpu(vm, (DWORD)_wtoi(value));
                else if (wcscmp(field, L"gpuMode") == 0) {
                    wchar_t gpu_id[512] = {0};
                    int mode = _wtoi(value);
                    if (!json_get_string(json, L"gpuId", gpu_id, ARRAYSIZE(gpu_id)) &&
                        json_has_key(json, L"gpuId"))
                        error = L"GPU selection is invalid or too long.";
                    else
                        error = asb_validate_gpu_selection(mode, gpu_id);
                    if (!error) hr = asb_vm_set_gpu_selection(vm, mode, gpu_id);
                }
                else if (wcscmp(field, L"networkMode") == 0) hr = asb_vm_set_network(vm, _wtoi(value));
                if (FAILED(hr)) ui_show_alert(error ? error : L"VM configuration could not be updated.");
                else asb_save();
            }
        } else {
            ui_show_alert(L"VM configuration could not be updated.");
        }
        send_vm_list();
    } else if (wcscmp(action, L"setDisplayMode") == 0) {
        /* {vmId, resolution: "adaptive"|"WxH", hz} from the VM's Display tab. */
        int idx, hz = 0, i;
        wchar_t res[32];
        VmInstance *v;
        VmDisplayIdd *display = NULL;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count() &&
            json_get_string(json, L"resolution", res, ARRAYSIZE(res)) &&
            json_get_int(json, L"hz", &hz) &&
            (v = asb_vm_instance(asb_vm_get(idx))) != NULL &&
            (_wcsicmp(v->os_type, L"Windows") == 0 || _wcsicmp(v->os_type, L"Linux") == 0)) {
            for (i = 0; i < ASB_MAX_VMS; i++)
                if (g_display_vm_ids[i] == v->unique_id && g_idd_displays[i] &&
                    vm_display_idd_is_open(g_idd_displays[i]))
                    display = g_idd_displays[i];
            if (!vm_display_idd_set_mode(display, v->vhdx_path, res, (UINT)hz))
                ui_show_alert(L"That display mode is not available.");
        }
        send_vm_list();
    } else if (wcscmp(action, L"ejectIso") == 0) {
        int idx;
        if (json_vm_index(json, &idx) && asb_vm_eject_iso(asb_vm_get(idx)) == E_ACCESSDENIED)
            ui_alert(L"info", L"Shut the VM down first", L"Shut the VM down first; the installer ISO is removed for the next start.");
        send_vm_list();
    } else if (wcscmp(action, L"getVmIcon") == 0) {
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL)
            vm_icon_post(v);
    } else if (wcscmp(action, L"setVmIcon") == 0 || wcscmp(action, L"clearVmIcon") == 0) {
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            if (wcscmp(action, L"clearVmIcon") == 0) {
                wchar_t path[MAX_PATH];
                vm_icon_file(v, L"icon.png", path, MAX_PATH);
                DeleteFileW(path);
                vm_icon_file(v, L"icon.ico", path, MAX_PATH);
                DeleteFileW(path);
            } else {
                OPENFILENAMEW ofn;
                wchar_t file[MAX_PATH] = L"", err[256] = L"";
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = g_hwnd_main;
                ofn.lpstrFilter = L"Images (*.png, *.ico)\0*.png;*.ico\0All Files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = L"Choose an icon for this VM";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (open_file_dialog(&ofn) && !vm_icon_set(v, file, err, ARRAYSIZE(err)))
                    ui_alert(L"error", L"Icon not changed", err);
            }
            vm_icon_post(v);
            {
                int k;
                for (k = 0; k < ASB_MAX_VMS; k++)
                    if (g_display_vm_ids[k] == v->unique_id && g_idd_displays[k])
                        vm_display_idd_icon_changed(g_idd_displays[k]);
            }
        }
        send_vm_list();
    } else if (wcscmp(action, L"createShortcut") == 0) {
        /* {vmId, where: "desktop"|"start"} */
        int idx;
        wchar_t where[16] = L"desktop", path[MAX_PATH * 2], msg[MAX_PATH * 2 + 200];
        VmInstance *v;
        json_get_string(json, L"where", where, ARRAYSIZE(where));
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count() &&
            (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            BOOL start_menu = wcscmp(where, L"start") == 0;
            HRESULT hr = create_vm_shortcut(v->name, start_menu, path, ARRAYSIZE(path), v);
            if (SUCCEEDED(hr)) {
                ui_log(L"Shortcut for \"%s\" created: %s", v->name, path);
                swprintf_s(msg, ARRAYSIZE(msg), start_menu
                    ? L"\"%s\" is now in the Start menu (App Sandbox folder). You can pin it to Start or the taskbar from there."
                    : L"A shortcut to \"%s\" is on your desktop. Opening it starts the VM and shows its window.",
                    v->name);
                ui_notice(L"info", msg);
            } else {
                swprintf_s(msg, ARRAYSIZE(msg), L"The shortcut could not be created (0x%08X).", (unsigned)hr);
                ui_alert(L"error", L"Shortcut not created", msg);
            }
        }
    } else if (wcscmp(action, L"setVideoCodec") == 0 || wcscmp(action, L"setVideoBitrate") == 0) {
        /* {vmId, codec: "auto"|"h264"|"hevc"} or {vmId, mbps} */
        int idx, i, mbps = 0;
        wchar_t codec[16] = L"";
        VmInstance *v;
        VmDisplayIdd *display = NULL;
        BOOL is_codec = wcscmp(action, L"setVideoCodec") == 0;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count() &&
            (v = asb_vm_instance(asb_vm_get(idx))) != NULL &&
            _wcsicmp(v->os_type, L"Windows") == 0) {
            for (i = 0; i < ASB_MAX_VMS; i++)
                if (g_display_vm_ids[i] == v->unique_id && g_idd_displays[i] &&
                    vm_display_idd_is_open(g_idd_displays[i]))
                    display = g_idd_displays[i];
            if (is_codec) {
                if (json_get_string(json, L"codec", codec, ARRAYSIZE(codec)))
                    vm_display_idd_set_codec(display, v->vhdx_path, codec);
            } else if (json_get_int(json, L"mbps", &mbps) && mbps >= 0 && mbps <= 500) {
                vm_display_idd_set_bitrate(display, v->vhdx_path, (UINT)mbps);
            }
        }
        send_vm_list();
    } else if (wcscmp(action, L"setViewerPrefs") == 0) {
        /* {titleStats, toolbar} from Settings > VM windows */
        BOOL title = asb_get_viewer_title_stats(), toolbar = asb_get_viewer_toolbar();
        int i;
        json_get_bool(json, L"titleStats", &title);
        json_get_bool(json, L"toolbar", &toolbar);
        asb_set_viewer_prefs(title, toolbar);
        {
            int renderer = 0;
            if (json_get_int(json, L"renderer", &renderer) && (renderer == 11 || renderer == 12))
                asb_set_viewer_renderer(renderer);
        }
        asb_save();
        for (i = 0; i < ASB_MAX_VMS; i++)
            if (g_idd_displays[i]) vm_display_idd_prefs_changed(g_idd_displays[i]);
        send_host_info();
    } else if (wcscmp(action, L"setPerformanceMode") == 0) {
        /* {vmId, enabled}: GPU video (games) vs. raw frames, per VM. */
        int idx, i;
        BOOL enabled = FALSE;
        VmInstance *v;
        VmDisplayIdd *display = NULL;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count() &&
            json_get_bool(json, L"enabled", &enabled) &&
            (v = asb_vm_instance(asb_vm_get(idx))) != NULL &&
            _wcsicmp(v->os_type, L"Windows") == 0) {
            for (i = 0; i < ASB_MAX_VMS; i++)
                if (g_display_vm_ids[i] == v->unique_id && g_idd_displays[i] &&
                    vm_display_idd_is_open(g_idd_displays[i]))
                    display = g_idd_displays[i];
            vm_display_idd_set_performance(display, v->vhdx_path, enabled);
        }
        send_vm_list();
    } else if (wcscmp(action, L"updateGuestTools") == 0) {
        int idx;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count()) {
            HRESULT hr = asb_vm_update_guest_tools(asb_vm_get(idx));
            if (hr == E_ACCESSDENIED)
                ui_alert(L"info", L"Shut the VM down first", L"Stop the VM (Shut down) before updating its guest components.");
            else if (FAILED(hr))
                ui_alert(L"info", L"Not available", L"Guest components can only be updated for Windows VMs.");
        }
        send_vm_list();
    } else if (wcscmp(action, L"exportVm") == 0) {
        /* Ask for a destination folder, then export there in the background. */
        int idx;
        if (json_vm_index(json, &idx) && idx >= 0 && idx < asb_vm_count()) {
            BROWSEINFOW browse;
            PIDLIST_ABSOLUTE selection;
            wchar_t path[MAX_PATH] = { 0 };
            ZeroMemory(&browse, sizeof(browse));
            browse.hwndOwner = g_hwnd_main;
            browse.lpszTitle = L"Choose where to save the exported VM (a <name>.asbvm folder is created there)";
            browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
            selection = SHBrowseForFolderW(&browse);
            if (selection) {
                if (SHGetPathFromIDListW(selection, path))
                    asb_vm_export(asb_vm_get(idx), path);
                CoTaskMemFree(selection);
            }
        }
        send_vm_list();
    } else if (wcscmp(action, L"importVm") == 0) {
        /* An export's vm.json (or a VM folder's disk.vhdx), or any .vhdx. */
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd_main;
        ofn.lpstrTitle = L"Import a VM: choose its vm.json (App Sandbox export) or a .vhdx disk";
        ofn.lpstrFilter = L"App Sandbox export or disk (vm.json, *.vhdx)\0vm.json;*.vhdx\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (g_exit_waiting)
            ui_notice(L"info", L"App Sandbox is shutting its VMs down to exit. Cancel that first.");
        else if (open_file_dialog(&ofn))
            asb_vm_import(file);
        send_vm_list();
    } else if (wcscmp(action, L"setAutoUpdateGuestTools") == 0) {
        BOOL enabled = TRUE;
        json_get_bool(json, L"enabled", &enabled);
        asb_set_auto_update_guest_tools(enabled);
        asb_save();
        send_host_info();
    } else if (wcscmp(action, L"setCloseBehavior") == 0) {
        /* {toTray: true} = hide to the tray without asking; false = ask. */
        BOOL to_tray = FALSE;
        json_get_bool(json, L"toTray", &to_tray);
        asb_set_suppress_tray_warn(to_tray);
        asb_save();
        send_host_info();
    } else if (wcscmp(action, L"selectVm") == 0) {
    } else if (wcscmp(action, L"getDiskSpace") == 0) {
        wchar_t *copy = _wcsdup(json);
        HANDLE thread = copy ? CreateThread(NULL, 0, disk_space_thread, copy, 0, NULL) : NULL;
        if (thread) CloseHandle(thread);
        else {
            free(copy);
            send_disk_space(json, FALSE);
        }
    } else if (wcscmp(action, L"browseDiskDirectory") == 0) {
        BROWSEINFOW browse;
        PIDLIST_ABSOLUTE selection;
        wchar_t initial[MAX_PATH] = {0}, path[MAX_PATH] = {0};
        json_get_string(json, L"path", initial, MAX_PATH);
        if (!initial[0]) asb_default_disk_directory(initial, MAX_PATH);
        ZeroMemory(&browse, sizeof(browse));
        browse.hwndOwner = g_hwnd_main;
        browse.lpszTitle = L"Choose a folder for VM disks";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        browse.lpfn = disk_folder_browse_callback;
        browse.lParam = (LPARAM)initial;
        selection = SHBrowseForFolderW(&browse);
        if (selection) {
            if (SHGetPathFromIDListW(selection, path)) {
                wchar_t json_buf[2048];
                JsonBuilder jb;
                jb_init(&jb, json_buf, 2048);
                jb_object_begin(&jb);
                jb_string(&jb, L"type", L"diskDirectoryBrowseResult");
                jb_string(&jb, L"path", path);
                jb_object_end(&jb);
                webview2_post(json_buf);
            }
            CoTaskMemFree(selection);
        }
    } else if (wcscmp(action, L"browseImage") == 0) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd_main;
        ofn.lpstrFilter = L"ISO Files (*.iso)\0*.iso\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (open_file_dialog(&ofn)) {
            wchar_t json_buf[2048];
            JsonBuilder jb;
            asb_set_last_iso_path(file);
            jb_init(&jb, json_buf, 2048);
            jb_object_begin(&jb);
            jb_string(&jb, L"type", L"browseResult");
            jb_string(&jb, L"path", file);
            jb_object_end(&jb);
            webview2_post(json_buf);
        }
    } else if (wcscmp(action, L"snapTake") == 0) {
        int vi;
        wchar_t sname[128] = {0};
        if (json_vm_index(json, &vi)) {
            json_get_string(json, L"name", sname, 128);
            notify_snapshot_failure(L"The snapshot could not be taken", asb_snap_take(asb_vm_get(vi), sname));
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapDelete") == 0) {
        int vi, si;
        if (json_vm_index(json, &vi) && json_get_int(json, L"snapIndex", &si)) {
            notify_snapshot_failure(L"The snapshot could not be deleted", asb_snap_delete(asb_vm_get(vi), si));
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapDeleteBranch") == 0) {
        int vi, si, bi;
        if (json_vm_index(json, &vi) && json_get_int(json, L"snapIndex", &si) &&
            json_get_int(json, L"branchIndex", &bi)) {
            notify_snapshot_failure(L"The branch could not be deleted", asb_snap_delete_branch(asb_vm_get(vi), si, bi));
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapRename") == 0) {
        int vi, si, bi = -1;
        wchar_t new_name[128];
        if (json_vm_index(json, &vi) && json_get_int(json, L"snapIndex", &si) &&
            json_get_string(json, L"name", new_name, 128)) {
            json_get_int(json, L"branchIndex", &bi);
            notify_snapshot_failure(L"The name could not be changed", asb_snap_rename(asb_vm_get(vi), si, bi, new_name));
            send_vm_list();
        }
    } else if (wcscmp(action, L"enableFeature") == 0) {
        HANDLE h = CreateThread(NULL, 0, enable_feature_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    } else if (wcscmp(action, L"enableFeatureReboot") == 0) {
        prereq_reboot();
    } else if (wcscmp(action, L"getState") == 0) {
        send_full_state();
    } else if (wcscmp(action, L"setMinSize") == 0) {
        /* {width, height} in CSS pixels; scaled with the window's DPI. */
        int cw = 0, ch = 0;
        json_get_int(json, L"width", &cw);
        json_get_int(json, L"height", &ch);
        if (cw > 0 && ch > 0 && cw < 10000 && ch < 10000) {
            RECT wr;
            SIZE min;
            g_min_css_width = cw;
            g_min_css_height = ch;
            min = min_window_size(g_hwnd_main);
            if (!g_borderless_fullscreen && GetWindowRect(g_hwnd_main, &wr)) {
                int cur_w = wr.right - wr.left;
                int cur_h = wr.bottom - wr.top;
                if (cur_w < min.cx || cur_h < min.cy) {
                    SetWindowPos(g_hwnd_main, NULL, 0, 0,
                        cur_w < min.cx ? min.cx : cur_w,
                        cur_h < min.cy ? min.cy : cur_h,
                        SWP_NOMOVE | SWP_NOZORDER);
                }
            }
        }
    } else if (wcscmp(action, L"openFolder") == 0) {
        /* {which: "vms" | "data" | "log" | "app"}: folders App Sandbox owns. */
        wchar_t which[16] = L"", path[MAX_PATH], message[MAX_PATH + 160];
        BOOL ok = FALSE;
        json_get_string(json, L"which", which, ARRAYSIZE(which));
        if (wcscmp(which, L"vms") == 0) {
            asb_default_disk_directory(path, MAX_PATH);
            ok = explorer_open_folder(path);
        } else if (wcscmp(which, L"data") == 0) {
            asb_program_data_dir(path, MAX_PATH);
            ok = explorer_open_folder(path);
        } else if (wcscmp(which, L"log") == 0) {
            log_file_path(path, MAX_PATH);
            ok = path_is_file(path) && explorer_select(path);
            if (!ok) {
                wchar_t *slash = wcsrchr(path, L'\\');
                if (slash) *slash = 0;
                ok = explorer_open_folder(path);
            }
        } else if (wcscmp(which, L"app") == 0) {
            if (asb_running_packaged()) {
                ui_notice(L"info", L"App Sandbox was installed from the Microsoft Store; Windows keeps its program folder private.");
                return;
            }
            if (GetModuleFileNameW(NULL, path, MAX_PATH)) ok = explorer_select(path);
            else path[0] = 0;
        } else {
            return;
        }
        if (!ok) {
            _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"This folder could not be opened: %s", path);
            ui_notice(L"error", message);
        }
    } else if (wcscmp(action, L"openVmFolder") == 0 || wcscmp(action, L"copyVmPath") == 0) {
        /* The VM's folder from vms.cfg: shown in Explorer (its disk selected)
           or copied to the clipboard. */
        int idx;
        VmInstance *v;
        wchar_t folder[MAX_PATH], message[MAX_PATH + 200];
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            if (!vm_root_folder(v, folder, MAX_PATH)) {
                ui_notice(L"error", L"This VM has no disk folder yet.");
            } else if (wcscmp(action, L"copyVmPath") == 0) {
                BOOL copied = clipboard_set_text(folder);
                if (copied)
                    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, L"Copied: %s", folder);
                else
                    wcscpy_s(message, ARRAYSIZE(message), L"The path could not be copied to the clipboard.");
                ui_notice(copied ? L"info" : L"error", message);
            } else {
                wchar_t disk_dir[MAX_PATH], *slash;
                BOOL ok;
                wcscpy_s(disk_dir, MAX_PATH, v->vhdx_path);
                slash = wcsrchr(disk_dir, L'\\');
                if (slash) *slash = 0;
                ok = (_wcsicmp(disk_dir, folder) == 0 && path_is_file(v->vhdx_path))
                    ? explorer_select(v->vhdx_path) : explorer_open_folder(folder);
                if (!ok) {
                    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                                 L"The folder of \"%s\" was not found: %s", v->name, folder);
                    ui_notice(L"error", message);
                }
            }
        }
    } else if (wcscmp(action, L"copyDiagnostics") == 0) {
        size_t cap = 32768;
        wchar_t *text = (wchar_t *)malloc(cap * sizeof(wchar_t));
        if (text) {
            build_diagnostics(text, cap);
            if (clipboard_set_text(text))
                ui_notice(L"info", L"Diagnostic info copied. Paste it into your bug report; it has no passwords or account names.");
            else
                ui_notice(L"error", L"The diagnostic info could not be copied to the clipboard.");
            free(text);
        }
    } else if (wcscmp(action, L"openLink") == 0) {
        /* {which}: one of the fixed project links (see open_project_link). */
        wchar_t which[16] = L"";
        if (json_get_string(json, L"which", which, ARRAYSIZE(which)))
            open_project_link(which);
    } else if (wcscmp(action, L"showDocument") == 0) {
        /* {which: "notices" | "license"}: the copy beside AppSandbox.exe, or
           the one on the project page when the package has none. */
        wchar_t which[16] = L"";
        BOOL notices;
        wchar_t *text;
        json_get_string(json, L"which", which, ARRAYSIZE(which));
        notices = wcscmp(which, L"notices") == 0;
        if (!notices && wcscmp(which, L"license") != 0) return;
        text = read_text_beside_exe(notices ? L"THIRD-PARTY-NOTICES.md" : L"LICENSE");
        if (text) {
            post_text_message(L"document", which, notices ? L"Third-party notices" : L"License", text);
            free(text);
        } else {
            open_project_link(which);
        }
    } else if (wcscmp(action, L"dismissVmError") == 0) {
        int idx;
        VmInstance *v;
        if (json_vm_index(json, &idx) && (v = asb_vm_instance(asb_vm_get(idx))) != NULL) {
            vm_note_clear(v->unique_id);
            send_vm_list();
        }
    } else if (wcscmp(action, L"cancelExit") == 0) {
        if (g_hwnd_main) exit_cancel(g_hwnd_main);
    }
}

/* ---- Library callbacks (bridge library events → UI thread) ---- */

static void CALLBACK ui_log_callback(const wchar_t *message, void *user_data)
{
    (void)user_data;
    if (GetCurrentThreadId() == g_ui_thread_id) {
        ui_log_post(message);
    } else if (g_hwnd_main) {
        size_t len = wcslen(message) + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, len, message);
            PostMessageW(g_hwnd_main, WM_WEBVIEW2_LOG, 0, (LPARAM)copy);
        }
    }
}

static void ui_core_event(const CoreVmEvent *event, void *context)
{
    (void)context;
    if (event->type == CORE_VM_EVENT_LOG || event->type == CORE_VM_EVENT_ALERT) {
        wchar_t message[16384] = {0};
        if (event->str_value)
            MultiByteToWideChar(CP_UTF8, 0, event->str_value, -1, message, ARRAYSIZE(message));
        if (event->type == CORE_VM_EVENT_LOG) ui_log_callback(message, NULL);
        else ui_show_alert(message);
    } else ui_refresh_from_core();
}

static void on_webview2_message(const wchar_t *json)
{
    wchar_t action[64] = { 0 };
    json_get_string(json, L"action", action, ARRAYSIZE(action));
    if (!action_is_passive(action)) {
        if (g_command_depth || asb_api_busy()) {
            reply_rejected(action, L"Another VM operation is in progress. Try again when it finishes.");
            return;
        }
        if (g_core_starting) {
            reply_rejected(action, L"App Sandbox is still starting. Try again in a moment.");
            return;
        }
        if (g_bg_op) {
            reply_rejected(action, L"App Sandbox is finishing a VM operation (shut down, force stop or delete). Try again in a moment.");
            return;
        }
    }
    ++g_command_depth;
    reconcile_displays();
    handle_webview2_message(json);
    --g_command_depth;
    ui_refresh_from_core();
}

/* ---- Window creation ---- */

HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    BOOL dark = TRUE;

    g_ui_thread_id = GetCurrentThreadId();
    g_hInstance = hInstance;
    g_is_home_edition = detect_home_edition();
    asb_set_hinstance(hInstance);

    g_core_subscription = asb_events_subscribe(ui_core_event, NULL);
    g_show_main_msg = RegisterWindowMessageW(UI_SHOW_MAIN_MESSAGE);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = main_wnd_proc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = UI_MAIN_WINDOW_CLASS;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX));

    if (!RegisterClassExW(&wc))
        return NULL;

    /* Be told early when Windows signs out or restarts, so running VMs can
       be shut down before the process (and with it every VM) ends. */
    SetProcessShutdownParameters(0x3FF, 0);

    hwnd = CreateWindowExW(
        0, UI_MAIN_WINDOW_CLASS, L"App Sandbox",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, UI_DEFAULT_WIDTH, UI_DEFAULT_HEIGHT,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
        return NULL;

    /* The default size is in 100%-scaling pixels: scale it to the monitor's
       DPI, keep it inside the work area and center it there. */
    {
        MONITORINFO mi;
        UINT dpi = GetDpiForWindow(hwnd);
        mi.cbSize = sizeof(mi);
        if (!dpi) dpi = 96;
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
            int w = MulDiv(UI_DEFAULT_WIDTH, (int)dpi, 96), h = MulDiv(UI_DEFAULT_HEIGHT, (int)dpi, 96);
            if (w > work_w * 9 / 10) w = work_w * 9 / 10;
            if (h > work_h * 9 / 10) h = work_h * 9 / 10;
            SetWindowPos(hwnd, NULL, mi.rcWork.left + (work_w - w) / 2, mi.rcWork.top + (work_h - h) / 2,
                         w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    g_hwnd_main = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    return hwnd;
}

/* ---- Window procedure ---- */

static LRESULT main_wnd_proc_handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Counts nested calls: a handler that dispatches messages while it waits
   (a viewer teardown, an HCS call, a dialog) re-enters the window
   procedure, and exit_may_run keeps the exit sequence out of those. */
static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result;
    ++g_wndproc_depth;
    result = main_wnd_proc_handle(hwnd, msg, wp, lp);
    --g_wndproc_depth;
    return result;
}

static LRESULT main_wnd_proc_handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_show_main_msg && msg == g_show_main_msg) {
        /* Another launch found this instance running (for example after the
           window was closed to the tray while VMs keep running). */
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 1;
    }

    switch (msg) {

    case WM_CREATE:
        hcs_set_monitor_hwnd(hwnd);
        vm_agent_set_hwnd(hwnd);
        asb_idd_probe_set_hwnd(hwnd);
        webview2_set_message_callback(on_webview2_message);
        webview2_set_accelerator_callback(on_webview2_accelerator);
        if (!webview2_init(hwnd, g_hInstance)) {
            MessageBoxW(hwnd, L"WebView2 initialization failed.\nPlease install Microsoft Edge WebView2 Runtime.",
                        L"App Sandbox", MB_ICONERROR);
        }
        tray_add(hwnd);
        /* Let a second launch reach this window even when integrity levels
           differ (UIPI would otherwise drop the message). */
        if (g_show_main_msg)
            ChangeWindowMessageFilterEx(hwnd, g_show_main_msg, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
        return 0;

    case WM_SIZE:
        webview2_resize(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == IDT_LAUNCH_VM) { launch_vm_tick(hwnd); return 0; }
        if (wp == IDT_EXIT_WAIT) {
            if (exit_may_run()) exit_tick(hwnd);
            return 0;
        }
        if (wp == IDT_OPEN_CONSOLE) {
            VmInstance *v = asb_find_vm_by_id(g_console_vm_id);
            int idx = v ? asb_vm_index((AsbVm)v) : -1;
            if (!v || ++g_console_tries > 60) {
                KillTimer(hwnd, IDT_OPEN_CONSOLE);
            } else if (v->running && !v->starting) {
                KillTimer(hwnd, IDT_OPEN_CONSOLE);
                do_connect_vm(idx);
            }
            return 0;
        }
        break;

    case WM_COPYDATA: {
        /* A VM shortcut was opened while this instance runs. */
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == UI_COPYDATA_LAUNCH_VM && cds->lpData &&
            cds->cbData >= sizeof(wchar_t) && cds->cbData <= 256 * sizeof(wchar_t)) {
            wchar_t name[256];
            size_t n = cds->cbData / sizeof(wchar_t);
            memcpy(name, cds->lpData, n * sizeof(wchar_t));
            name[n - 1] = 0;
            ui_launch_vm_by_name(name);
            return TRUE;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1L << 30))) {
            toggle_borderless_fullscreen(hwnd);
            return 0;
        }
        break;

    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            tray_handle_measure((MEASUREITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            tray_handle_draw((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        SIZE min = min_window_size(hwnd);
        if (min.cx > 0) mmi->ptMinTrackSize.x = min.cx;
        if (min.cy > 0) mmi->ptMinTrackSize.y = min.cy;
        return 0;
    }

    case WM_DPICHANGED:
        /* Moved to a monitor with another scale: take the size Windows suggests. */
        if (!g_borderless_fullscreen) {
            const RECT *suggested = (const RECT *)lp;
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_ASB_API_REQUEST:
        /* A VM operation runs on the worker thread: the request waits for it. */
        if (g_bg_op && !g_destroying && g_deferred_api_count < (int)ARRAYSIZE(g_deferred_api)) {
            g_deferred_api[g_deferred_api_count++] = lp;
            return 0;
        }
        return asb_api_dispatch((void *)lp);

    case WM_ASB_API_SHUTDOWN:
        DestroyWindow(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
    {
        /* Signing out, restarting, or the package is being updated. Ending
           the process turns every VM off, so hold Windows back with a reason
           while the VMs shut down, and exit when they have stopped. Windows
           shows the reason and the user can still continue at once. */
        int running = 0, busy = 0;
        if (!g_prereq_ok || (lp & ENDSESSION_CRITICAL)) return TRUE;
        count_vm_activity(&running, &busy);
        if (!running) return TRUE;
        if (!g_exit_block_reason)
            g_exit_block_reason = ShutdownBlockReasonCreate(hwnd, L"Shutting down virtual machines so their work is saved");
        exit_begin(hwnd, TRUE);
        return FALSE;
    }

    case WM_ENDSESSION:
        /* wp FALSE: the sign-out or restart was cancelled; App Sandbox stays.
           wp TRUE: the session ends now with whatever still runs. */
        if (!wp && g_exit_for_session) exit_cancel(hwnd);
        return 0;

    case WM_CORE_CHANGED:
        InterlockedExchange(&g_refresh_pending, 0);
        /* The worker is still loading the VM list; WM_CORE_READY sends it. */
        if (g_core_starting) return 0;
        if (g_command_depth || asb_api_busy()) return 0;
        reconcile_displays();
        send_vm_list();
        send_templates();
        return 0;

    case WM_VM_AGENT_STATUS:
    {
        reconcile_displays();
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst && inst->running && inst->agent_online && !inst->display_suppressed && !inst->shutdown_requested) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Agent online - switching \"%s\" from RDP to IDD.", inst->name);
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                } else if (!g_idd_displays[i] && !inst->shutdown_requested) {
                    do_connect_idd(i);
                }
                break;
            }
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_IDD_READY:
    {
        reconcile_displays();
        /* lp is a stable per-VM id (UINT64), not a VmInstance*. Resolve
           fresh; returns NULL if the VM was deleted between PostMessage
           and our processing. */
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst && inst->running && !inst->install_complete && !inst->shutdown_requested && !inst->display_suppressed) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (!g_idd_displays[i]) {
                    ui_log(L"VDD ready - opening IDD display for \"%s\".", inst->name);
                    do_connect_idd(i);
                }
                break;
            }
        }
        return 0;
    }

    case WM_VM_HYPERV_VIDEO_OFF:
    {
        reconcile_displays();
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst && inst->running && !inst->display_suppressed) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Hyper-V Video disabled - switching to IDD.");
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                }
                break;
            }
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_DISPLAY_CLOSED:
    {
        reconcile_displays();
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (wp && wp == vm_display_token(g_displays[i])) safe_destroy_rdp(i);
                else if (wp && wp == vm_display_idd_token(g_idd_displays[i])) safe_destroy_idd(i);
                else break;
                asb_vm_set_display_suppressed((AsbVm)inst, TRUE);
                asb_events_changed();
                break;
            }
        }
        return 0;
    }

    case WM_VM_SHUTDOWN_TIMEOUT:
    {
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        ULONGLONG elapsed = (ULONGLONG)wp;
        if (inst && inst->running && elapsed % 30 < 3)
            ui_log(L"WARNING: VM \"%s\" still shutting down (%llu seconds).", inst->name, elapsed);
        return 0;
    }

    case WM_VM_AGENT_GPUCOPY:
    {
        reconcile_displays();
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i]) {
                    safe_destroy_rdp(i);
                    ui_log(L"RDP display torn down for \"%s\" (GPU driver activation).", inst->name);
                }
                break;
            }
        }
        return 0;
    }

    case WM_WEBVIEW2_LOG:
    {
        wchar_t *log_text = (wchar_t *)lp;
        if (log_text) { ui_log_post(log_text); free(log_text); }
        return 0;
    }

    case WM_DISK_SPACE:
    {
        wchar_t *response = (wchar_t *)lp;
        if (response) {
            webview2_post(response);
            HeapFree(GetProcessHeap(), 0, response);
        }
        return 0;
    }

    case WM_PREREQ_PROGRESS:
    {
        wchar_t buf[128];
        _snwprintf_s(buf, 128, _TRUNCATE,
            L"{\"type\":\"prereqProgress\",\"pct\":%d}", (int)wp);
        webview2_post(buf);
        return 0;
    }

    case WM_PREREQ_DONE:
    {
        BOOL ok = (BOOL)wp;
        BOOL reboot_required = (BOOL)lp;
        if (ok && !reboot_required) {
            /* Feature enabled, no reboot needed - initialize now */
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":true,\"reboot\":false}");
            start_core_async();
        } else if (ok && reboot_required) {
            g_prereq_reboot_pending = TRUE;
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":true,\"reboot\":true}");
        } else {
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":false,\"reboot\":false}");
        }
        return 0;
    }

    case WM_CORE_READY:
        InterlockedExchange(&g_core_starting, 0);
        if (!wp) {
            webview2_post(L"{\"type\":\"prereqRequired\"}");
            return 0;
        }
        g_prereq_ok = TRUE;
        if (!asb_api_start(g_hInstance, g_hwnd_main))
            ui_alert(L"warning", L"Local API not available", L"The local API could not start. See headless.log for details.");
        send_full_state();
        webview2_post(L"{\"type\":\"coreReady\"}");
        return 0;

    case WM_SHOW_ALERT:
    {
        PendingAlert *pa = (PendingAlert *)lp;
        if (pa) {
            ui_alert(pa->kind, pa->title[0] ? pa->title : NULL, pa->message);
            free(pa);
        }
        return 0;
    }

    case WM_BG_OP_DONE:
        /* Only for the operation in progress: WM_DESTROY frees it itself, and
           its teardown dispatches messages that may still include this one. */
        if (lp && !g_destroying && (BgOp *)lp == g_bg_op) bg_op_finish((BgOp *)lp);
        return 0;

    case WM_TRAYICON:
        if (g_command_depth || asb_api_busy()) return 0;
        if (lp == WM_LBUTTONDBLCLK || ((lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) && g_bg_op)) {
            /* While a VM operation finishes the menu's actions would wait;
               show the window instead. */
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) {
            ++g_command_depth;
            tray_show_menu(hwnd);
            --g_command_depth;
            ui_refresh_from_core();
        }
        return 0;

    case WM_CLOSE:
        if (g_command_depth || asb_api_busy()) return 0;
        if (g_bg_op) {
            ui_notice(L"info", L"App Sandbox is finishing a VM operation. Close the window again in a moment.");
            return 0;
        }
        confirm_exit(hwnd, TRUE);
        return 0;

    case WM_DESTROY:
    {
        int i;
        BOOL worker_stuck = FALSE;
        g_destroying = TRUE;
        KillTimer(hwnd, IDT_EXIT_WAIT);
        if (g_exit_block_reason) {
            ShutdownBlockReasonDestroy(hwnd);
            g_exit_block_reason = FALSE;
        }
        /* Let a stop or delete in progress finish before the core shuts down. */
        if (g_bg_thread) {
            if (WaitForSingleObject(g_bg_thread, 60000) == WAIT_OBJECT_0) {
                MSG done;
                /* Its WM_BG_OP_DONE may already be queued: drop it, then
                   free the operation here. */
                while (PeekMessageW(&done, hwnd, WM_BG_OP_DONE, WM_BG_OP_DONE, PM_REMOVE)) {}
                free(g_bg_op);
                g_bg_op = NULL;
            } else {
                /* Still inside a core call: the core is not cleaned up
                   under it. Ending the process closes what is left. */
                worker_stuck = TRUE;
            }
            CloseHandle(g_bg_thread);
            g_bg_thread = NULL;
        }
        /* Waiting API requests are answered while the API stops. */
        for (i = 0; i < g_deferred_api_count; i++)
            PostMessageW(hwnd, WM_ASB_API_REQUEST, 0, g_deferred_api[i]);
        g_deferred_api_count = 0;
        asb_api_stop();
        asb_events_unsubscribe(g_core_subscription);
        g_core_subscription = 0;
        tray_remove();
        webview2_cleanup();
        for (i = 0; i < ASB_MAX_VMS; i++) {
            safe_destroy_rdp(i);
            safe_destroy_idd(i);
        }
        if (!worker_stuck) asb_cleanup();
        g_hwnd_main = NULL;
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
