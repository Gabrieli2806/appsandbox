/*
 * appsandbox-agent.exe — Guest-side Windows service for AppSandbox.
 *
 * Listens on a Hyper-V socket (AF_HYPERV) for commands from the host.
 * Maintains a persistent connection: sends "hello" on connect, then
 * periodic "heartbeat" messages. Processes commands inline.
 *
 * Also handles GPU driver file copy via embedded 9P client (p9copy)
 * when the host sends gpu_query_response with share metadata.
 *
 * Supports: ping, shutdown, restart, gpu_copy, gpu_query_response,
 *           gpu_none, idd_connect, set_ip.
 *
 * Usage:
 *   appsandbox-agent.exe --install   Install and start the service
 *   appsandbox-agent.exe --remove    Stop and remove the service
 *   (no args)                        Run as Windows service (SCM only)
 */

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <winevt.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include "p9copy.h"
#include "gl_vk_provision.h"
#include "../transport/asb_transport.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wevtapi.lib")

/* GUID_DEVCLASS_DISPLAY = {4D36E968-E325-11CE-BFC1-08002BE10318} */
static const GUID GUID_DISPLAY_CLASS =
    { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* DEVPKEY_Device_DriverInfPath = {A8B865DD-2E3D-4094-AD97-E593A70C75D6}, 5 */
static const DEVPROPKEY DPKEY_DriverInfPath =
    { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* HV_GUID_WILDCARD = {00000000-0000-0000-0000-000000000000} */
static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Must match VM_AGENT_SERVICE_GUID_STR in vm_agent.h
   {A5B0CAFE-0001-4000-8000-000000000001} */
static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Service constants ---- */

#define SERVICE_NAME    "AppSandboxAgent"
#define DISPLAY_NAME    "AppSandbox Guest Agent"

static SERVICE_STATUS        g_status;
static SERVICE_STATUS_HANDLE g_status_handle;
static HANDLE                g_stop_event;
/* Active persistent connection. Set and cleared only under g_send_cs; other
   threads reach it only through send_to_host(). */
static AsbConn *             g_client_sock = NULL;
static volatile BOOL         g_os_shutting_down = FALSE;
/* Serializes send_line and guards g_client_sock. Like the other locks and
   events below, it lives until the process exits: threads that are still
   finishing (GPU copy, device checks, ViGEm setup) may use it after the
   service reports itself stopped. */
static CRITICAL_SECTION      g_send_cs;

/* ---- Logging ---- */

static int send_line(AsbConn *s, const char *msg);

/* Send one line to the connected host from any thread. g_send_cs is held
   across the check and the send, and handle_client clears g_client_sock under
   the same lock before it closes the connection, so the connection cannot be
   freed while a send is using it. The lock is held only for the send itself.
   Returns the send result, or 0 when no host is connected. */
static int send_to_host(const char *line)
{
    int n = 0;
    EnterCriticalSection(&g_send_cs);
    if (g_client_sock != NULL)
        n = send_line(g_client_sock, line);
    LeaveCriticalSection(&g_send_cs);
    return n;
}

static BOOL host_connected(void)
{
    BOOL connected;
    EnterCriticalSection(&g_send_cs);
    connected = g_client_sock != NULL;
    LeaveCriticalSection(&g_send_cs);
    return connected;
}

#define AGENT_LOG_PATH     "C:\\Windows\\AppSandbox\\agent.log"
#define AGENT_LOG_OLD_PATH "C:\\Windows\\AppSandbox\\agent.old.log"
/* Past this size agent.log becomes agent.old.log (replacing the previous
   one) and a new agent.log is started. */
#define AGENT_LOG_MAX_BYTES (8LL * 1024 * 1024)

/* Every thread writes agent.log through agent_log, and so do p9copy and
   gl_vk_provision (p9_set_log, gl_vk_set_log): one whole line per append,
   under this lock. The file is opened with full sharing, so a reader that
   has it open does not make a line disappear. */
static SRWLOCK g_log_lock = SRWLOCK_INIT;

static HANDLE open_agent_log(void)
{
    return CreateFileA(AGENT_LOG_PATH, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Appends one time-stamped line to agent.log. The caller's last error is
   preserved, so a failing call can be logged before its error is read. */
static void agent_log(const char *fmt, ...)
{
    char line[2048];
    int len, n;
    va_list ap;
    SYSTEMTIME st;
    HANDLE f;
    LARGE_INTEGER size;
    DWORD written, error = GetLastError();

    GetLocalTime(&st);
    len = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (len < 0) len = 0;
    va_start(ap, fmt);
    /* Two bytes stay free for the line end. */
    n = _vsnprintf_s(line + len, sizeof(line) - len - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    len = n < 0 ? (int)strlen(line) : len + n;
    line[len++] = '\r';
    line[len++] = '\n';

    AcquireSRWLockExclusive(&g_log_lock);
    f = open_agent_log();
    if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &size) &&
        size.QuadPart > AGENT_LOG_MAX_BYTES) {
        CloseHandle(f);
        MoveFileExA(AGENT_LOG_PATH, AGENT_LOG_OLD_PATH, MOVEFILE_REPLACE_EXISTING);
        f = open_agent_log();
    }
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, line, (DWORD)len, &written, NULL);
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
    SetLastError(error);
}

/* Send a log message to the host via the command channel (non-recursive, won't call agent_log).
   Returns the send result: > 0 when the line was sent, otherwise no host
   was connected or the send failed. */
static int agent_log_to_host(const char *fmt, ...)
{
    char msg[1024];
    char line[1040];
    va_list ap;

    if (!host_connected()) return 0;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    snprintf(line, sizeof(line), "log:%s", msg);
    line[sizeof(line) - 1] = '\0';
    return send_to_host(line);
}

/* ---- GPU copy state ---- */

#define MAX_GPU_SHARES 64

typedef struct {
    char share_name[128];
    char dest_path[512];    /* wchar_t as UTF-8 — converted when used */
    char filter[4096];
} GpuShareInfo;

/* Progress, done and error lines go through send_to_host(), so a copy that
   outlives its connection reports to whichever host connection is current. */
typedef struct {
    GpuShareInfo shares[MAX_GPU_SHARES];
    int          count;
    volatile BOOL copying;
} GpuCopyState;

static GpuCopyState g_gpu_copy = {0};

/* Forward declarations — defined later but needed by GPU copy */
static int recv_line(AsbConn *s, char *buf, int buf_size);
static BOOL enable_privilege(LPCWSTR priv_name);
static void gpu_shares_received(void);

/* ---- GPU copy background thread ---- */

/* Check if any Display class device using vrd.inf has error code 43.
   Returns TRUE if at least one vrd.inf device has problem 43. */
static BOOL check_gpu_error43(void)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    BOOL found_error43 = FALSE;

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("GPU check: SetupDiGetClassDevs failed (%lu).", GetLastError());
        return FALSE;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        /* Get driver inf path */
        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0)) {
            continue;
        }

        /* Only care about vrd.inf (GPU-PV virtual render driver) */
        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        agent_log("GPU check: found vrd.inf device: %ls", dev_id);

        cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU check: CM_Get_DevNode_Status failed (%lu) for %ls.", cr, dev_id);
            continue;
        }

        if (problem == 43) {
            agent_log("GPU check: device %ls has error 43.", dev_id);
            found_error43 = TRUE;
        } else {
            agent_log("GPU check: device %ls OK (status=0x%lx, problem=%lu).",
                      dev_id, status, problem);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found_error43;
}

static void disable_hyperv_video(BOOL notify_host);


/* Disable and re-enable vrd.inf display devices.
   If only_error43 is TRUE, only cycles devices with problem code 43.
   If FALSE, cycles all vrd.inf devices (used at preshutdown).
   With notify_host, each step is reported to the connected host. */
static void cycle_gpu_devices(BOOL notify_host, BOOL only_error43)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    char msg[512];

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return;

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0))
            continue;

        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        if (only_error43) {
            cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
            if (cr != CR_SUCCESS || problem != 43)
                continue;
        }

        agent_log("GPU cycle: disabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:disabling %ls", dev_id);
        if (notify_host) send_to_host(msg);

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Disable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:disable failed (%lu)", cr);
            if (notify_host) send_to_host(msg);
            continue;
        }

        Sleep(1000);

        agent_log("GPU cycle: re-enabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabling %ls", dev_id);
        if (notify_host) send_to_host(msg);

        cr = CM_Enable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Enable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:enable failed (%lu)", cr);
            if (notify_host) send_to_host(msg);
            continue;
        }

        agent_log("GPU cycle: device %ls re-enabled.", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabled %ls", dev_id);
        if (notify_host) send_to_host(msg);
    }

    SetupDiDestroyDeviceInfoList(devs);
}

/* WTS constants — defined manually to avoid wtsapi32.h dependency (dynamic load) */
#define MY_WTS_USERNAME 5  /* WTS_INFO_CLASS value for WTSUserName */

/* Wait for a real user to log in (not defaultuser0 or SYSTEM).
   Polls WTS sessions every 2 seconds up to timeout_ms.
   Returns the session ID, or 0xFFFFFFFF on timeout. */
static DWORD wait_for_user_login(AsbConn *notify_sock, int timeout_ms,
                                  const char *notify_msg)
{
    typedef BOOL (WINAPI *PFN_WTSQuerySessionInformationW)(
        HANDLE, DWORD, DWORD, LPWSTR *, DWORD *);
    typedef void (WINAPI *PFN_WTSFreeMemory)(PVOID);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSQuerySessionInformationW pfnQuery;
    PFN_WTSFreeMemory pfnFree;
    DWORD start, now;

    if (!wts) {
        agent_log("wait_for_user_login: cannot load wtsapi32.dll.");
        return 0xFFFFFFFF;
    }

    pfnQuery = (PFN_WTSQuerySessionInformationW)
        GetProcAddress(wts, "WTSQuerySessionInformationW");
    pfnFree = (PFN_WTSFreeMemory)GetProcAddress(wts, "WTSFreeMemory");

    if (!pfnQuery || !pfnFree) {
        agent_log("wait_for_user_login: WTS functions not found.");
        FreeLibrary(wts);
        return 0xFFFFFFFF;
    }

    agent_log("Waiting for user login...");
    if (notify_sock != NULL && notify_msg)
        send_line(notify_sock, notify_msg);

    start = GetTickCount();

    for (;;) {
        DWORD session_id = WTSGetActiveConsoleSessionId();
        if (session_id != 0xFFFFFFFF) {
            LPWSTR username = NULL;
            DWORD size = 0;

            if (pfnQuery(NULL, session_id, MY_WTS_USERNAME, &username, &size)) {
                if (username && wcslen(username) > 0 &&
                    _wcsicmp(username, L"defaultuser0") != 0 &&
                    _wcsicmp(username, L"SYSTEM") != 0) {
                    agent_log("User '%ls' logged in (session %lu).", username, session_id);
                    pfnFree(username);
                    FreeLibrary(wts);
                    return session_id;
                }
                if (username) pfnFree(username);
            }
        }

        now = GetTickCount();
        if ((now - start) >= (DWORD)timeout_ms) {
            agent_log("wait_for_user_login: timed out after %d ms.", timeout_ms);
            FreeLibrary(wts);
            return 0xFFFFFFFF;
        }

        Sleep(2000);
    }
}

/* Log off a specific user session via WTSLogoffSession. */
static BOOL logoff_session(DWORD session_id)
{
    typedef BOOL (WINAPI *PFN_WTSLogoffSession)(HANDLE, DWORD, BOOL);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSLogoffSession pfnLogoff;
    BOOL result;

    if (!wts) return FALSE;

    pfnLogoff = (PFN_WTSLogoffSession)GetProcAddress(wts, "WTSLogoffSession");
    if (!pfnLogoff) {
        FreeLibrary(wts);
        return FALSE;
    }

    agent_log("Logging off session %lu...", session_id);
    result = pfnLogoff(NULL, session_id, TRUE /* bWait */);
    if (!result)
        agent_log("WTSLogoffSession failed: %lu", GetLastError());
    else
        agent_log("Session %lu logged off.", session_id);

    FreeLibrary(wts);
    return result;
}

/* Run a command hidden and wait for it (used for takeown/icacls). The cmd buffer
   must be writable (CreateProcessW may modify it). */
static DWORD run_quiet(wchar_t *cmd)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD result;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
        if (wait == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(pi.hProcess, &result))
                result = GetLastError();
        } else {
            result = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        result = GetLastError();
    }
    return result;
}

/* Deploy the AppSandbox public key into administrators_authorized_keys -- the file
   Windows OpenSSH uses for members of the Administrators group (the guest account
   is an admin). sshd's StrictModes requires that file be writable only by the
   Administrators group + SYSTEM with inheritance removed, so we fix the ACL too. */
static BOOL deploy_ssh_key(const char *pubkey)
{
    wchar_t base[MAX_PATH], ssh_dir[MAX_PATH], authk[MAX_PATH], cmd[MAX_PATH + 256];
    FILE *f = NULL;

    if (!pubkey || !*pubkey) return FALSE;
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(ssh_dir, MAX_PATH, L"%s\\ssh", base);
    swprintf_s(authk, MAX_PATH, L"%s\\administrators_authorized_keys", ssh_dir);
    CreateDirectoryW(ssh_dir, NULL);

    if (_wfopen_s(&f, authk, L"w") != 0 || !f) {
        agent_log("SSH key: cannot open authorized_keys (%lu)", GetLastError());
        return FALSE;
    }
    fprintf(f, "%s\n", pubkey);
    fclose(f);

    swprintf_s(cmd, MAX_PATH + 256,
               L"icacls \"%s\" /inheritance:r /grant *S-1-5-32-544:F /grant *S-1-5-18:F",
               authk);
    run_quiet(cmd);
    agent_log("SSH key: deployed to administrators_authorized_keys");
    return TRUE;
}

static void gl_provision(const wchar_t *dir, const wchar_t *native_dir)
{
    wchar_t path[MAX_PATH], sys[MAX_PATH], dst[MAX_PATH];
    HKEY key;
    DWORD zero = 0;
    BOOL native_runtime = FALSE;

    agent_log("GL: provisioning GPU runtimes from %ls", dir[0] ? dir : native_dir);

    /* dxil.dll -> System32 (OpenGLOn12 / vulkan_dzn load it by leaf name). */
    if (dir[0] && GetSystemDirectoryW(sys, MAX_PATH)) {
        swprintf_s(path, MAX_PATH, L"%s\\dxil.dll", dir);
        swprintf_s(dst, MAX_PATH, L"%s\\dxil.dll", sys);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(path, dst, FALSE))
                agent_log("GL: staged dxil.dll to System32.");
            else
                agent_log("GL: dxil.dll -> System32 failed (%lu).", GetLastError());
        }
    }

    if (GetSystemDirectoryW(sys, MAX_PATH)) {
        if (gl_vk_provision_runtime(dir, native_dir, sys, &native_runtime))
            agent_log("GL: OpenGL runtime provisioned in System32.");
        else
            agent_log("GL: opengl32 -> System32 failed (%lu).", GetLastError());
    } else {
        agent_log("GL: GetSystemDirectory failed; OpenGL not deployed.");
    }

    /* OpenCL: Khronos vendor key — value name = ICD path, data 0 (= load it). */
    if (dir[0]) {
        swprintf_s(path, MAX_PATH, L"%s\\OpenCLOn12.dll", dir);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenCL\\Vendors",
                    0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
                RegSetValueExW(key, path, 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
                RegCloseKey(key);
                agent_log("GL: registered OpenCL ICD %ls", path);
            }
        }
    }

    /* Vulkan: Khronos driver key — value name = ICD manifest path.
       The manifest's library_path is the absolute guest DLL path (set host-side
       in d3dlayers.c) so the loader can LoadLibraryEx it under
       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, which requires a fully-qualified path. */
    if (dir[0]) {
        swprintf_s(path, MAX_PATH, L"%s\\dzn_icd.json", dir);
    } else {
        UINT length = GetWindowsDirectoryW(dst, MAX_PATH);
        if (!length || length >= MAX_PATH ||
            swprintf_s(path, MAX_PATH, L"%s\\AppSandbox\\d3dlayers\\dzn_icd.json", dst) < 0)
            return;
    }
    if (native_runtime || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        DWORD disabled = native_runtime ? 1 : 0;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers",
                0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(key, path, 0, REG_DWORD, (const BYTE *)&disabled, sizeof(disabled));
            RegCloseKey(key);
            if (!native_runtime) agent_log("GL: registered Vulkan (Dozen) ICD %ls", path);
        }
    }

    agent_log("GL: provisioning complete.");
}

/* NVIDIA's installer makes DRS writable by desktop and packaged applications.
   Plan9 transfers file bytes, not the host ACL, so establish these permissions
   on the guest before seeding the profile database. */
static BOOL nvidia_drs_prepare(const wchar_t *dir)
{
    wchar_t parent[MAX_PATH], sys[MAX_PATH], cmd[MAX_PATH * 2 + 160];
    wchar_t *slash;
    DWORD err, attrs;

    wcscpy_s(parent, MAX_PATH, dir);
    slash = wcsrchr(parent, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    if (!CreateDirectoryW(parent, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        agent_log("NVIDIA DRS: cannot create parent directory (%lu).", GetLastError());
        return FALSE;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        agent_log("NVIDIA DRS: cannot create directory (%lu).", GetLastError());
        return FALSE;
    }
    attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        agent_log("NVIDIA DRS: destination is not an accessible directory.");
        return FALSE;
    }
    if (!GetSystemDirectoryW(sys, MAX_PATH)) {
        agent_log("NVIDIA DRS: GetSystemDirectory failed (%lu).", GetLastError());
        return FALSE;
    }

    /* Grant Everyone and ALL APPLICATION PACKAGES inherited full control
       within DRS. Numeric SIDs work with every guest language; preserve ACEs. */
    swprintf_s(cmd, MAX_PATH * 2 + 160,
               L"\"%s\\icacls.exe\" \"%s\" /grant *S-1-1-0:(OI)(CI)F "
               L"*S-1-15-2-1:(OI)(CI)F /T /C /Q", sys, dir);
    err = run_quiet(cmd);
    if (err != ERROR_SUCCESS) {
        agent_log("NVIDIA DRS: setting guest permissions failed (%lu).", err);
        return FALSE;
    }
    return TRUE;
}

/* Agent state kept in the registry (clean-shutdown marker, last reported
   crash dump, per-boot flags). */
#define AGENT_REG_KEY L"SOFTWARE\\AppSandbox\\Agent"

/* TRUE only the first time it is called for 'name' in this Windows boot.
   The marker is a volatile registry key, which Windows drops at restart, so
   a restart of this service within the same boot still sees it. Falls back
   to a per-process answer when the registry cannot be used. */
static BOOL first_time_this_boot(const wchar_t *name, volatile LONG *process_flag)
{
    HKEY parent, key;
    DWORD disposition = 0;
    BOOL first = InterlockedExchange(process_flag, 1) == 0;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, 0, NULL, 0,
                        KEY_CREATE_SUB_KEY, NULL, &parent, NULL) != ERROR_SUCCESS)
        return first;
    if (RegCreateKeyExW(parent, name, 0, NULL, REG_OPTION_VOLATILE,
                        KEY_READ, NULL, &key, &disposition) == ERROR_SUCCESS) {
        RegCloseKey(key);
        first = disposition == REG_CREATED_NEW_KEY;
    }
    RegCloseKey(parent);
    return first;
}

/* The AppSandbox.Nvidia share holds only the NVIDIA shims, and every one of
   them is in its refresh filter: they are re-copied on every connect, so
   files from this share say nothing about the GPU driver itself. */
#define GPU_SHIM_SHARE "AppSandbox.Nvidia"

static DWORD WINAPI gpu_copy_thread(LPVOID param)
{
    static volatile LONG gpu_cycled_flag = 0;
    GpuCopyState *state = (GpuCopyState *)param;
    int i;
    int total_files = 0;
    int driver_files = 0;   /* files copied outside the always-refreshed shim share */
    int pending_files = 0;  /* new copies that wait for the next restart (destination in use) */
    int failed_shares = 0;
    char msg[256];
    wchar_t gl_dir[MAX_PATH], native_dir[MAX_PATH];

    gl_dir[0] = 0;
    native_dir[0] = 0;
    agent_log("GPU copy starting (%d shares)...", state->count);

    for (i = 0; i < state->count; i++) {
        GpuShareInfo *si = &state->shares[i];
        wchar_t dest_wide[MAX_PATH];
        int files = 0;
        int pending = 0;
        int rc;

        dest_wide[0] = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, si->dest_path, -1, dest_wide, MAX_PATH) == 0) {
            agent_log("GPU copy share '%s': dest path conversion failed (%lu), skipping.",
                      si->share_name, GetLastError());
            failed_shares++;
            continue;
        }

        /* The GL mapping-layer share is provisioned after the copy loop. */
        if (strcmp(si->share_name, "AppSandbox.GlLayers") == 0)
            wcscpy_s(gl_dir, MAX_PATH, dest_wide);

        agent_log("GPU copy share %d/%d: %s -> %s%s%s",
                  i + 1, state->count, si->share_name, si->dest_path,
                  si->filter[0] ? " [filter: " : "",
                  si->filter[0] ? si->filter : "");

        if (strcmp(si->share_name, "AppSandbox.NvidiaDrs") == 0) {
            /* This list is also sent after reconnects. Seed missing files;
               existing profiles belong to the guest. The host's runtime
               lock file is recreated locally by NVIDIA when needed. */
            const P9CopyOptions options = { TRUE, "nvdrswr.lk", NULL, NULL, NULL, &pending };
            if (nvidia_drs_prepare(dest_wide))
                rc = p9_copy_share_ex(50001, si->share_name, dest_wide,
                                     si->filter[0] ? si->filter : NULL,
                                     &options, &files);
            else
                rc = P9_ERR_IO;
        } else if (strcmp(si->share_name, GPU_SHIM_SHARE) == 0) {
            const P9CopyOptions options = {
                FALSE, NULL, "appsandbox-nvidia-vk-gl-shim.dll;appsandbox-nvidia-vk-gl-shim32.dll;"
                             "appsandbox-nvidia-dlss-shim.dll;appsandbox-nvidia-cuda-shim.dll;"
                             "appsandbox-nvidia-opencl-shim.dll",
                NULL, NULL, &pending
            };
            rc = p9_copy_share_ex(50001, si->share_name, dest_wide,
                                 si->filter[0] ? si->filter : NULL,
                                 &options, &files);
        } else if (strncmp(si->share_name, "AppSandbox.Drv.", 15) == 0) {
            const P9CopyOptions options = {
                FALSE, NULL, NULL, nvidia_opencl_copy_target, nvidia_opencl_commit_copy, &pending
            };
            rc = p9_copy_share_ex(50001, si->share_name, dest_wide,
                                 si->filter[0] ? si->filter : NULL,
                                 &options, &files);
        } else {
            const P9CopyOptions options = { FALSE, NULL, NULL, NULL, NULL, &pending };
            rc = p9_copy_share_ex(50001, si->share_name, dest_wide,
                                 si->filter[0] ? si->filter : NULL,
                                 &options, &files);
        }

        if (rc != P9_OK) {
            char line[256];
            agent_log("GPU copy share '%s' failed (rc=%d).", si->share_name, rc);
            sprintf_s(line, sizeof(line), "log:GPU driver sync: '%s' failed (error %d)",
                      si->share_name, rc);
            send_to_host(line);
            failed_shares++;
        } else {
            agent_log("GPU copy share '%s' done (%d files).", si->share_name, files);
            if (strcmp(si->share_name, GPU_SHIM_SHARE) == 0)
                wcscpy_s(native_dir, MAX_PATH, dest_wide);
        }
        total_files += files;
        pending_files += pending;
        if (strcmp(si->share_name, GPU_SHIM_SHARE) != 0)
            driver_files += files;

        /* Send progress to host */
        sprintf_s(msg, sizeof(msg), "gpu_copy_progress:%d/%d", i + 1, state->count);
        send_to_host(msg);
    }

    /* Files whose destination was in use are only replaced when Windows
       restarts, so until then the guest runs a mix of old and new driver
       files. Restarting the GPU device cannot change that (and disturbs the
       display): say that a restart is needed instead.
       Otherwise, if driver files were copied and vrd.inf has error 43,
       restart the GPU device, at most once per boot: the device restart
       itself can disturb the display, and a second one in the same boot
       does not help. */
    if (pending_files > 0) {
        char line[256];
        agent_log("GPU driver sync: %d file(s) in use will be replaced at the next restart; "
                  "not restarting the GPU device.", pending_files);
        sprintf_s(line, sizeof(line),
                  "log:GPU driver sync: %d driver file(s) are in use; restart the VM to finish the update",
                  pending_files);
        send_to_host(line);
    } else if (driver_files > 0 && check_gpu_error43()) {
        if (first_time_this_boot(L"GpuCycledThisBoot", &gpu_cycled_flag)) {
            agent_log("GPU drivers copied and error 43 detected - attempting device restart.");
            cycle_gpu_devices(TRUE, TRUE);

            /* TODO: re-enable once IDD display is stable
            Sleep(2000);
            disable_hyperv_video(TRUE);
            */
        } else {
            agent_log("GPU drivers copied and error 43 detected, but the GPU device was "
                      "already restarted in this boot - not restarting it again.");
        }
    } else if (driver_files > 0) {
        agent_log("GPU drivers copied, no error 43 - no device restart needed.");
    } else if (total_files > 0) {
        agent_log("Only the NVIDIA shims were refreshed - no device restart needed.");
    } else {
        agent_log("All GPU driver files already present (pre-staged) - no copy or restart needed.");
    }

    if (failed_shares == 0 && (gl_dir[0] || native_dir[0] || gpu_prefers_system_opengl()))
        gl_provision(gl_dir, native_dir);
    /* The NVIDIA runtime setup (CUDA, OpenCL, NVAPI, DLSS) is optional and
       not a share: a failure is reported once and the GPU sync still counts
       as done. gl_vk_provision.c logs the step that failed. */
    if (failed_shares == 0 && !nvidia_runtime_provision(native_dir)) {
        agent_log("NVIDIA runtime setup skipped: provisioning failed (details above).");
        send_to_host("log:NVIDIA runtime setup skipped (see C:\\Windows\\AppSandbox\\agent.log)");
    }

    /* Send final result to host */
    if (failed_shares == 0) {
        sprintf_s(msg, sizeof(msg), "gpu_copy_done:%d", total_files);
        agent_log("GPU copy complete: %d files, %d shares.", total_files, state->count);
    } else {
        sprintf_s(msg, sizeof(msg), "gpu_copy_error:%d/%d shares failed",
                  failed_shares, state->count);
        agent_log("GPU copy finished with errors: %d/%d shares failed.",
                  failed_shares, state->count);
    }

    send_to_host(msg);

    state->copying = FALSE;
    return 0;
}

/* Read and drop the share lines of a gpu_query_response that is not used,
   so they are not taken for commands. Hosts send at most a few dozen. */
static void discard_share_lines(AsbConn *client, int share_count)
{
    int i;
    if (share_count > 1024) share_count = 1024;
    for (i = 0; i < share_count; i++) {
        char line[8192];
        if (recv_line(client, line, sizeof(line)) <= 0) {
            agent_log("GPU query: failed to read share line %d.", i);
            return;
        }
    }
}

/* Parse gpu_query_response and start background copy.
   Format: "gpu_query_response:N" followed by N lines of "share|dest|filter"
   (filter may be empty). */
static void handle_gpu_query_response(AsbConn *client, int share_count)
{
    int i;
    HANDLE thread;

    if (share_count <= 0 || share_count > MAX_GPU_SHARES) {
        agent_log("GPU query response: invalid share count %d.", share_count);
        discard_share_lines(client, share_count);
        return;
    }

    if (g_gpu_copy.copying) {
        /* The running copy reports its result to this connection too. */
        agent_log("GPU copy already in progress, ignoring.");
        discard_share_lines(client, share_count);
        return;
    }

    g_gpu_copy.count = 0;

    for (i = 0; i < share_count; i++) {
        char line[8192];
        int n = recv_line(client, line, sizeof(line));
        if (n <= 0) {
            agent_log("GPU query: failed to read share line %d.", i);
            return;
        }

        /* Parse "share_name|dest_path|filter" */
        {
            char *p1, *p2;
            GpuShareInfo *si = &g_gpu_copy.shares[g_gpu_copy.count];

            p1 = strchr(line, '|');
            if (!p1) {
                agent_log("GPU query: malformed share line: %s", line);
                continue;
            }
            *p1 = '\0';
            p1++;

            p2 = strchr(p1, '|');
            if (p2) {
                *p2 = '\0';
                p2++;
                strncpy_s(si->filter, sizeof(si->filter), p2, _TRUNCATE);
            } else {
                si->filter[0] = '\0';
            }

            strncpy_s(si->share_name, sizeof(si->share_name), line, _TRUNCATE);
            strncpy_s(si->dest_path, sizeof(si->dest_path), p1, _TRUNCATE);
            g_gpu_copy.count++;

            agent_log("GPU share [%d]: %s -> %s%s%s",
                      g_gpu_copy.count, si->share_name, si->dest_path,
                      si->filter[0] ? " filter=" : "",
                      si->filter[0] ? si->filter : "");
        }
    }

    if (g_gpu_copy.count == 0) {
        agent_log("GPU query: no valid shares to copy.");
        send_line(client, "gpu_copy_done:0");
        return;
    }
    gpu_shares_received();

    /* Start background copy */
    g_gpu_copy.copying = TRUE;
    thread = CreateThread(NULL, 0, gpu_copy_thread, &g_gpu_copy, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else {
        agent_log("Failed to create GPU copy thread (error %lu).", GetLastError());
        g_gpu_copy.copying = FALSE;
        send_line(client, "gpu_copy_error:thread creation failed");
    }
}

/* ---- Line I/O ---- */

static int recv_line(AsbConn *s, char *buf, int buf_size)
{
    int pos = 0;
    while (pos < buf_size - 1) {
        char c;
        int n = asb_recv(s, &c, 1);
        if (n <= 0) return n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int send_line(AsbConn *s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    EnterCriticalSection(&g_send_cs);
    n = asb_send(s, msg, len);
    if (n > 0) n = asb_send(s, "\n", 1);
    LeaveCriticalSection(&g_send_cs);
    return n;
}

/* ---- Privilege helper ---- */

static BOOL enable_privilege(LPCWSTR priv_name)
{
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, priv_name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

/* ---- Input helper process (spawned into console session as SYSTEM) ---- */

/* Protects g_input_process / g_input_session.  The connection thread
   (idd_connect) and the input monitor thread both kill and respawn the
   helper, so the handle is only taken out of the global under this lock
   and closed by whoever took it -- never twice. */
static CRITICAL_SECTION g_input_proc_cs;

static HANDLE g_input_process = NULL;
static DWORD  g_input_session = 0xFFFFFFFF;  /* session the helper was spawned into */
static volatile BOOL g_input_monitor_running = FALSE;
static HANDLE g_input_monitor_thread = NULL;

static void kill_input_helper(void)
{
    HANDLE proc;

    EnterCriticalSection(&g_input_proc_cs);
    proc = g_input_process;
    g_input_process = NULL;
    g_input_session = 0xFFFFFFFF;
    LeaveCriticalSection(&g_input_proc_cs);

    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

/* Spawn appsandbox-input.exe as SYSTEM in the given session.
   Duplicates our own token, sets the session ID, then CreateProcessAsUser. */
static BOOL spawn_input_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    /* Build path to appsandbox-input.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-input.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Input helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Input helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Input helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Input helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Input helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Input helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    {
        /* If the other thread spawned a helper meanwhile, the newer one
           replaces it: only one helper may own the input channel. */
        HANDLE older;
        EnterCriticalSection(&g_input_proc_cs);
        older = g_input_process;
        g_input_process = pi.hProcess;
        g_input_session = session_id;
        LeaveCriticalSection(&g_input_proc_cs);
        if (older) {
            TerminateProcess(older, 0);
            WaitForSingleObject(older, 3000);
            CloseHandle(older);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

/* Monitor thread: every 3 seconds, check if the console session changed
   or if the helper process died.  Respawn as needed. */
static DWORD WINAPI input_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Input monitor: started.");

    while (g_input_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            HANDLE dead = NULL;
            DWORD  sess_snap;
            BOOL helper_alive;

            /* Check liveness and take a dead helper's handle in the same
               locked step: idd_connect may kill and respawn the helper at
               any time, and a new process handle can reuse the old value. */
            EnterCriticalSection(&g_input_proc_cs);
            sess_snap = g_input_session;
            helper_alive = g_input_process &&
                WaitForSingleObject(g_input_process, 0) != WAIT_OBJECT_0;
            if (g_input_process && !helper_alive && cur_session == sess_snap) {
                dead = g_input_process;
                g_input_process = NULL;
            }
            LeaveCriticalSection(&g_input_proc_cs);

            if (cur_session != sess_snap) {
                /* Console session changed — kill old helper and respawn */
                if (helper_alive) {
                    agent_log("Input monitor: console session changed %lu -> %lu, respawning.",
                               sess_snap, cur_session);
                    kill_input_helper();
                }
                spawn_input_in_session(cur_session);
            } else if (!helper_alive) {
                /* Same session but no live helper (it died, or none was
                   started) — respawn. */
                if (dead) {
                    CloseHandle(dead);
                    agent_log("Input monitor: helper died, respawning in session %lu.", cur_session);
                }
                spawn_input_in_session(cur_session);
            }
        }

        /* Sleep 3 seconds, but check stop flag every 500ms */
        {
            int i;
            for (i = 0; i < 6 && g_input_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_input_helper();
    agent_log("Input monitor: stopped.");
    return 0;
}

static void start_input_monitor(void)
{
    g_input_monitor_running = TRUE;
    g_input_monitor_thread = CreateThread(NULL, 0, input_monitor_thread, NULL, 0, NULL);
}

static void stop_input_monitor(void)
{
    /* Called from the connection thread (shutdown/restart commands) and from
       service_main: whoever takes the thread handle closes it. */
    HANDLE thread = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile *)&g_input_monitor_thread, NULL);
    g_input_monitor_running = FALSE;
    if (thread) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
}

/* ---- Clipboard helper process (same pattern as input helper) ---- */

/* Protects the clipboard helper/reader process globals below.  Both the SCM
   control handler thread (SESSIONCHANGE LOGON/UNLOCK) and the clipboard
   monitor threads mutate these globals (kill/spawn plus the test-then-close),
   so without serialization the same HANDLE can be closed twice. */
static CRITICAL_SECTION g_clip_proc_cs;

static HANDLE g_clipboard_process = NULL;
static DWORD  g_clipboard_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_monitor_running = FALSE;
static HANDLE g_clipboard_monitor_thread = NULL;

static void kill_clipboard_helper(void)
{
    HANDLE proc;

    EnterCriticalSection(&g_clip_proc_cs);
    proc = g_clipboard_process;
    g_clipboard_process = NULL;
    g_clipboard_session = 0xFFFFFFFF;
    LeaveCriticalSection(&g_clip_proc_cs);

    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

static BOOL spawn_clipboard_in_session(DWORD session_id)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    LPVOID env = NULL;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Clipboard helper: WTSQueryUserToken(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        return FALSE;
    }

    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) {
        agent_log("Clipboard helper: CreateEnvironmentBlock failed (%lu).", GetLastError());
        env = NULL;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(user_token, exe_path, NULL, NULL, NULL,
                               FALSE,
                               CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                               env, NULL, &si, &pi)) {
        agent_log("Clipboard helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);
        return FALSE;
    }

    agent_log("Clipboard helper: spawned PID %lu in session %lu (as user).",
               pi.dwProcessId, session_id);
    EnterCriticalSection(&g_clip_proc_cs);
    g_clipboard_process = pi.hProcess;
    g_clipboard_session = session_id;
    LeaveCriticalSection(&g_clip_proc_cs);
    CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return TRUE;
}

static DWORD WINAPI clipboard_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard monitor: started.");

    while (g_clipboard_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            HANDLE proc_snap;
            DWORD  sess_snap;
            BOOL helper_alive;

            EnterCriticalSection(&g_clip_proc_cs);
            proc_snap = g_clipboard_process;
            sess_snap = g_clipboard_session;
            LeaveCriticalSection(&g_clip_proc_cs);

            helper_alive = proc_snap &&
                WaitForSingleObject(proc_snap, 0) != WAIT_OBJECT_0;

            if (cur_session != sess_snap) {
                if (helper_alive) {
                    agent_log("Clipboard monitor: console session changed %lu -> %lu, respawning.",
                               sess_snap, cur_session);
                    kill_clipboard_helper();
                }
                spawn_clipboard_in_session(cur_session);
            } else if (!helper_alive) {
                HANDLE dead;
                EnterCriticalSection(&g_clip_proc_cs);
                dead = g_clipboard_process;
                g_clipboard_process = NULL;
                LeaveCriticalSection(&g_clip_proc_cs);
                if (dead) CloseHandle(dead);
                agent_log("Clipboard monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_helper();
    agent_log("Clipboard monitor: stopped.");
    return 0;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = TRUE;
    g_clipboard_monitor_thread = CreateThread(NULL, 0, clipboard_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_monitor(void)
{
    g_clipboard_monitor_running = FALSE;
    if (g_clipboard_monitor_thread) {
        WaitForSingleObject(g_clipboard_monitor_thread, 5000);
        CloseHandle(g_clipboard_monitor_thread);
        g_clipboard_monitor_thread = NULL;
    }
}

/* ---- Clipboard reader process (runs as USER, :0006) ---- */

static HANDLE g_clipboard_reader_process = NULL;
static DWORD  g_clipboard_reader_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_reader_monitor_running = FALSE;
static HANDLE g_clipboard_reader_monitor_thread = NULL;

static void kill_clipboard_reader(void)
{
    HANDLE proc;

    EnterCriticalSection(&g_clip_proc_cs);
    proc = g_clipboard_reader_process;
    g_clipboard_reader_process = NULL;
    g_clipboard_reader_session = 0xFFFFFFFF;
    LeaveCriticalSection(&g_clip_proc_cs);

    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

static BOOL spawn_clipboard_reader_in_session(DWORD session_id)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    LPVOID env = NULL;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard-reader.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard reader: %ls not found.", exe_path);
        return FALSE;
    }

    /* Get the logged-in user's token — this runs the process as the user,
       which allows it to read the user's clipboard. */
    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Clipboard reader: WTSQueryUserToken(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        return FALSE;
    }

    /* Create environment block for the user */
    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) {
        agent_log("Clipboard reader: CreateEnvironmentBlock failed (%lu).", GetLastError());
        env = NULL;  /* proceed without it */
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(user_token, exe_path, NULL, NULL, NULL,
                               FALSE,
                               CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                               env, NULL, &si, &pi)) {
        agent_log("Clipboard reader: CreateProcessAsUserW failed (%lu).", GetLastError());
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);
        return FALSE;
    }

    agent_log("Clipboard reader: spawned PID %lu in session %lu (as user).",
               pi.dwProcessId, session_id);
    EnterCriticalSection(&g_clip_proc_cs);
    g_clipboard_reader_process = pi.hProcess;
    g_clipboard_reader_session = session_id;
    LeaveCriticalSection(&g_clip_proc_cs);
    CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return TRUE;
}

static DWORD WINAPI clipboard_reader_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard reader monitor: started.");

    while (g_clipboard_reader_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            HANDLE proc_snap;
            DWORD  sess_snap;
            BOOL helper_alive;

            EnterCriticalSection(&g_clip_proc_cs);
            proc_snap = g_clipboard_reader_process;
            sess_snap = g_clipboard_reader_session;
            LeaveCriticalSection(&g_clip_proc_cs);

            helper_alive = proc_snap &&
                WaitForSingleObject(proc_snap, 0) != WAIT_OBJECT_0;

            if (cur_session != sess_snap) {
                if (helper_alive) {
                    agent_log("Clipboard reader monitor: console session changed %lu -> %lu, respawning.",
                               sess_snap, cur_session);
                    kill_clipboard_reader();
                }
                spawn_clipboard_reader_in_session(cur_session);
            } else if (!helper_alive) {
                HANDLE dead;
                EnterCriticalSection(&g_clip_proc_cs);
                dead = g_clipboard_reader_process;
                g_clipboard_reader_process = NULL;
                LeaveCriticalSection(&g_clip_proc_cs);
                if (dead) CloseHandle(dead);
                agent_log("Clipboard reader monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_reader_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_reader_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_reader();
    agent_log("Clipboard reader monitor: stopped.");
    return 0;
}

static void start_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = TRUE;
    g_clipboard_reader_monitor_thread = CreateThread(NULL, 0, clipboard_reader_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = FALSE;
    if (g_clipboard_reader_monitor_thread) {
        WaitForSingleObject(g_clipboard_reader_monitor_thread, 5000);
        CloseHandle(g_clipboard_reader_monitor_thread);
        g_clipboard_reader_monitor_thread = NULL;
    }
}

/* ---- Audio capture helper process (SYSTEM in console session, :0004) ---- */

static HANDLE g_audio_process = NULL;
static DWORD  g_audio_session = 0xFFFFFFFF;
static volatile BOOL g_audio_monitor_running = FALSE;
static HANDLE g_audio_monitor_thread = NULL;

static void kill_audio_helper(void)
{
    if (g_audio_process) {
        TerminateProcess(g_audio_process, 0);
        WaitForSingleObject(g_audio_process, 3000);
        CloseHandle(g_audio_process);
        g_audio_process = NULL;
        g_audio_session = 0xFFFFFFFF;
    }
}

static BOOL spawn_audio_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-audio.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Audio helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Audio helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Audio helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Audio helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Audio helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Audio helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    g_audio_process = pi.hProcess;
    g_audio_session = session_id;
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

static DWORD WINAPI audio_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Audio monitor: started.");

    while (g_audio_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_audio_process &&
                WaitForSingleObject(g_audio_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_audio_session) {
                if (helper_alive) {
                    agent_log("Audio monitor: console session changed %lu -> %lu, respawning.",
                               g_audio_session, cur_session);
                    kill_audio_helper();
                }
                spawn_audio_in_session(cur_session);
            } else if (!helper_alive) {
                if (g_audio_process) {
                    CloseHandle(g_audio_process);
                    g_audio_process = NULL;
                }
                agent_log("Audio monitor: helper died, respawning in session %lu.", cur_session);
                spawn_audio_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_audio_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_audio_helper();
    agent_log("Audio monitor: stopped.");
    return 0;
}

static void start_audio_monitor(void)
{
    g_audio_monitor_running = TRUE;
    g_audio_monitor_thread = CreateThread(NULL, 0, audio_monitor_thread, NULL, 0, NULL);
}

static void stop_audio_monitor(void)
{
    g_audio_monitor_running = FALSE;
    if (g_audio_monitor_thread) {
        WaitForSingleObject(g_audio_monitor_thread, 5000);
        CloseHandle(g_audio_monitor_thread);
        g_audio_monitor_thread = NULL;
    }
}

/* ---- Helper: run devcon command and capture stdout ---- */

/* devcon waits for PnP, which can be busy for many seconds at boot or while
   drivers install. Queries and changes get separate time limits; a run that
   hits its limit is killed and reported with DEVCON_TIMED_OUT instead of an
   exit code, so callers never act on partial output. */
#define DEVCON_QUERY_TIMEOUT_MS   15000UL    /* status */
#define DEVCON_CHANGE_TIMEOUT_MS  60000UL    /* enable / disable / restart */
#define DEVCON_UPDATE_TIMEOUT_MS  120000UL   /* update (driver package install) */
#define DEVCON_TIMED_OUT          0xFFFFFFFFUL

/* Start cmd with stdout/stderr on 'out' (an inheritable pipe end).
   PROC_THREAD_ATTRIBUTE_HANDLE_LIST limits inheritance to that pipe, so the
   child does not get copies of the agent's sockets, pipes and process
   handles. Plain inheritance is used only when the attribute list cannot be
   built or CreateProcess rejects it (ERROR_INVALID_PARAMETER); any other
   failure (devcon missing, access denied) is returned as is, with
   GetLastError() set. */
static BOOL create_process_with_output(wchar_t *cmd, HANDLE out, PROCESS_INFORMATION *pi)
{
    STARTUPINFOEXW six;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T size = 0;
    HANDLE inherit[1];

    ZeroMemory(&six, sizeof(six));
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdOutput = out;
    six.StartupInfo.hStdError = out;
    six.StartupInfo.hStdInput = NULL;
    ZeroMemory(pi, sizeof(*pi));
    inherit[0] = out;

    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    if (size)
        attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, size);
    if (attrs) {
        if (InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
            BOOL tried = FALSE, ok = FALSE;
            DWORD err = ERROR_SUCCESS;
            if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          inherit, sizeof(inherit), NULL, NULL)) {
                six.StartupInfo.cb = sizeof(six);
                six.lpAttributeList = attrs;
                tried = TRUE;
                ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                    NULL, NULL, &six.StartupInfo, pi);
                if (!ok) err = GetLastError();
            }
            DeleteProcThreadAttributeList(attrs);
            HeapFree(GetProcessHeap(), 0, attrs);
            if (ok) return TRUE;
            if (tried && err != ERROR_INVALID_PARAMETER) {
                SetLastError(err);
                return FALSE;
            }
            if (tried)
                agent_log("devcon: start with a restricted handle list was rejected (%lu), "
                          "retrying without it.", err);
        } else {
            HeapFree(GetProcessHeap(), 0, attrs);
        }
    }

    six.StartupInfo.cb = sizeof(STARTUPINFOW);
    six.lpAttributeList = NULL;
    ZeroMemory(pi, sizeof(*pi));
    return CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                          NULL, NULL, &six.StartupInfo, pi);
}

/* Move whatever is waiting in the pipe into output, without blocking.
   Bytes beyond the buffer are read and dropped so the child never stalls
   on a full pipe. */
static void drain_pipe(HANDLE pipe, char *output, int output_size, int *pos)
{
    char scratch[512];
    DWORD avail, got;

    while (PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        int room = output_size - 1 - *pos;
        if (room > 0) {
            if (avail > (DWORD)room) avail = (DWORD)room;
            if (!ReadFile(pipe, output + *pos, avail, &got, NULL) || got == 0) break;
            *pos += (int)got;
        } else {
            if (avail > sizeof(scratch)) avail = sizeof(scratch);
            if (!ReadFile(pipe, scratch, avail, &got, NULL) || got == 0) break;
        }
    }
    output[*pos] = '\0';
}

/* Run devcon with args, capture its output and wait up to timeout_ms.
   Returns FALSE (GetLastError set) if devcon could not be started. On
   success *out_exit_code is devcon's exit code, or DEVCON_TIMED_OUT when it
   did not finish in time (it is killed; output holds what it printed). */
static BOOL run_devcon(const wchar_t *args, char *output, int output_size,
                       DWORD *out_exit_code, DWORD timeout_ms)
{
    PROCESS_INFORMATION pi;
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    wchar_t cmd[MAX_PATH * 2];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    DWORD start, exit_code = DEVCON_TIMED_OUT;
    BOOL timed_out = FALSE;
    int pos = 0;

    output[0] = '\0';
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0';

    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%sdrivers\\devcon.exe\" %s", exe_dir, args);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return FALSE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    if (!create_process_with_output(cmd, hWrite, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(hRead);
        CloseHandle(hWrite);
        SetLastError(err);
        return FALSE;
    }
    CloseHandle(hWrite);

    /* Read while waiting, so a long output cannot fill the pipe and stall
       devcon until the time limit. */
    start = GetTickCount();
    for (;;) {
        DWORD wait;
        drain_pipe(hRead, output, output_size, &pos);
        wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait != WAIT_TIMEOUT) break;
        if (GetTickCount() - start >= timeout_ms) {
            agent_log("run_devcon: '%ls' did not finish within %lu s, killing it.",
                      args, timeout_ms / 1000);
            TerminateProcess(pi.hProcess, DEVCON_TIMED_OUT);
            WaitForSingleObject(pi.hProcess, 3000);
            timed_out = TRUE;
            break;
        }
    }

    /* devcon has exited: everything it wrote is in the pipe. */
    drain_pipe(hRead, output, output_size, &pos);
    CloseHandle(hRead);

    if (!timed_out && !GetExitCodeProcess(pi.hProcess, &exit_code))
        exit_code = DEVCON_TIMED_OUT;
    if (out_exit_code) *out_exit_code = exit_code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

/* ---- IDD driver status check ---- */

/* What devcon status says about Root\AppSandboxVDD. */
typedef enum {
    VDD_RUNNING,        /* "Driver is running." */
    VDD_PROBLEM,        /* "Device has a problem: N." */
    VDD_DISABLED,       /* "Device is disabled." */
    VDD_STOPPED,        /* "Device is currently stopped." */
    VDD_NOT_FOUND,      /* "No matching devices found." */
    VDD_UNRECOGNISED,   /* any other output */
    VDD_NO_DEVCON,      /* devcon could not be started */
    VDD_TIMED_OUT       /* devcon did not finish in time: no reading */
} VddState;

/* The "idd_status:<value>" the host receives for each state. */
static const char *idd_status_name(VddState st)
{
    switch (st) {
    case VDD_RUNNING:   return "ok";
    case VDD_PROBLEM:   return "error";
    case VDD_DISABLED:  return "disabled";
    case VDD_NOT_FOUND:
    case VDD_NO_DEVCON: return "not_found";
    default:            return "unknown";
    }
}

/* The latest reading, for the status sent right after "hello": the state + 1
   in the low 4 bits, GetTickCount64() above them; 0 before the first one.
   A reading older than VDD_READING_MAX_AGE_MS is not sent (the device-check
   thread sends a live one). */
static volatile LONG64 g_vdd_last_reading;
#define VDD_READING_MAX_AGE_MS 120000ULL

/* Runs devcon status once (device-check thread only) and remembers the
   reading. output receives devcon's text. */
static VddState read_vdd_state(char *output, int output_size)
{
    DWORD exit_code = 0;
    VddState st;

    if (!run_devcon(L"status Root\\AppSandboxVDD", output, output_size, &exit_code,
                    DEVCON_QUERY_TIMEOUT_MS)) {
        agent_log("IDD status: devcon failed to launch (%lu).", GetLastError());
        output[0] = '\0';
        st = VDD_NO_DEVCON;
    } else if (exit_code == DEVCON_TIMED_OUT) {
        return VDD_TIMED_OUT;
    } else if (strstr(output, "running")) {
        st = VDD_RUNNING;
    } else if (strstr(output, "disabled")) {
        st = VDD_DISABLED;
    } else if (strstr(output, "problem")) {
        st = VDD_PROBLEM;
    } else if (strstr(output, "stopped")) {
        st = VDD_STOPPED;
    } else if (strstr(output, "No matching")) {
        st = VDD_NOT_FOUND;
    } else {
        st = VDD_UNRECOGNISED;
    }
    InterlockedExchange64(&g_vdd_last_reading,
                          (LONG64)((GetTickCount64() << 4) | (ULONGLONG)(st + 1)));
    return st;
}

/* The latest reading as an idd_status value, or NULL when there is none
   recent enough. Any thread. */
static const char *recent_idd_status(void)
{
    LONG64 reading = InterlockedCompareExchange64(&g_vdd_last_reading, 0, 0);
    ULONGLONG at;

    if (!reading) return NULL;
    at = (ULONGLONG)reading >> 4;
    if (GetTickCount64() - at > VDD_READING_MAX_AGE_MS) return NULL;
    return idd_status_name((VddState)((reading & 0xF) - 1));
}

/* Report "idd_status:<status>" (ok / error / disabled / not_found / unknown)
   for a reading. Called by the device-check thread when a host connects
   (force=1: always send, so a reconnect re-syncs the host's latched
   idd_ready) AND periodically (force=0: send only on change) -- so the
   host's display-readiness tracks the LIVE VDD state, which can self-heal
   via ensure_vdd_running(). The host re-latches every line, so the
   change-gate avoids spamming the daemon log/events. agent.log gets a line
   when the reading changes. */
static void report_idd_status(VddState st, const char *output, int force)
{
    static char last_sent[16];      /* last value the host received */
    static int  last_logged = -1;   /* last VddState written to agent.log */
    const char *name;

    if (st == VDD_TIMED_OUT) {
        /* No answer from PnP: nothing new to tell, except on connect, where
           the host waits for a first status. */
        agent_log("IDD status: devcon status timed out.");
        if (!force) return;
        name = "unknown";
    } else {
        name = idd_status_name(st);
        if ((int)st != last_logged) {
            if (st == VDD_RUNNING || st == VDD_DISABLED || st == VDD_NOT_FOUND)
                agent_log("IDD status: AppSandboxVDD %s.", st == VDD_RUNNING ? "running" :
                          st == VDD_DISABLED ? "disabled" : "not found");
            else if (st != VDD_NO_DEVCON)
                agent_log("IDD status: %s. Output: %.400s", name, output);
            last_logged = (int)st;
        }
    }

    /* Send only when the value changes (force overrides on connect); remember
       it only once the host has it. */
    if (force || strcmp(name, last_sent) != 0) {
        char line[32];
        sprintf_s(line, sizeof(line), "idd_status:%s", name);
        if (send_to_host(line) > 0)
            strcpy_s(last_sent, sizeof(last_sent), name);
    }
}

/* ---- Messages for the host log from the device-check thread ----
 * Results of work done while no host may be connected (staged display-driver
 * update, GPU crash workaround, a display driver that needs attention) are
 * sent at once when a host is connected, otherwise kept and sent when the
 * next host connects. Used only by the device-check thread. */
#define DEVICE_NOTES 4
static char g_device_notes[DEVICE_NOTES][600];

static void send_device_notes(void)
{
    int i;
    for (i = 0; i < DEVICE_NOTES; i++)
        if (g_device_notes[i][0] && agent_log_to_host("%s", g_device_notes[i]) > 0)
            g_device_notes[i][0] = '\0';
}

static void device_note(const char *fmt, ...)
{
    char text[600];
    va_list ap;
    int i;
    char *c;

    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    for (c = text; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';   /* one host-log line */
    for (i = 0; i < DEVICE_NOTES; i++) {
        if (!g_device_notes[i][0]) {
            strcpy_s(g_device_notes[i], sizeof(g_device_notes[i]), text);
            break;
        }
    }
    send_device_notes();
}

/* ---- VDD device check: restart if not running ---- */

/* ---- Display driver update staged by the host (guest_tools.c) ----
 *
 * With the VM stopped, App Sandbox copies a newer VDD package into
 * C:\Windows\AppSandbox\drivers and leaves vdd-update.pending there. Each
 * build signs the driver with its own test certificate, so trust that first,
 * then update the existing Root\AppSandboxVDD device to the new package. */
static void run_logged(const wchar_t *cmd_line, const char *what)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[1024];
    DWORD code = 1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    wcscpy_s(cmd, ARRAYSIZE(cmd), cmd_line);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("%s: could not start (%lu).", what, GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    agent_log("%s: exit %lu.", what, code);
}

/* Installs a display driver staged by the host. Runs at service start on
   the device-check thread; the result goes to the host log (device_note). */
static void apply_pending_vdd_update(void)
{
    wchar_t dir[MAX_PATH], marker[MAX_PATH], cer[MAX_PATH], sys_dir[MAX_PATH];
    wchar_t cmd[1024], *slash;
    char output[1024];
    char note[600] = "";
    DWORD exit_code = 0;
    /* devcon update's exit code: 0 done, 1 done but a restart is needed (the
       old driver file was still in use), 2 failed; DEVCON_TIMED_OUT when it
       was killed. Stays 2 if devcon could not be started. */
    DWORD update_code = 2;

    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0';
    swprintf_s(marker, MAX_PATH, L"%s\\drivers\\vdd-update.pending", dir);
    if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES) return;

    agent_log("VDD update: package staged by the host, installing.");
    /* Remove the marker first: if the install crashes or hangs this service,
       the next start must not try again in a loop. The host log shows how to
       retry (Update guest components). */
    DeleteFileW(marker);
    GetSystemDirectoryW(sys_dir, MAX_PATH);
    swprintf_s(cer, MAX_PATH, L"%s\\drivers\\AppSandboxVDD.cer", dir);
    if (GetFileAttributesW(cer) != INVALID_FILE_ATTRIBUTES) {
        swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\\certutil.exe\" -addstore Root \"%s\"", sys_dir, cer);
        run_logged(cmd, "VDD update: trust certificate (Root)");
        swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\\certutil.exe\" -f -addstore TrustedPublisher \"%s\"", sys_dir, cer);
        run_logged(cmd, "VDD update: trust certificate (TrustedPublisher)");
    }
    /* Stop the device first so WUDFHost releases AppSandboxVDD.dll and the
       new file is copied now instead of being left for a later reboot. */
    if (run_devcon(L"disable Root\\AppSandboxVDD", output, sizeof(output), &exit_code,
                   DEVCON_CHANGE_TIMEOUT_MS)) {
        if (exit_code == DEVCON_TIMED_OUT)
            agent_log("VDD update: disable timed out: %.200s", output);
        else
            agent_log("VDD update: disable (exit %lu): %.200s", exit_code, output);
    }
    swprintf_s(cmd, ARRAYSIZE(cmd), L"update \"%s\\drivers\\AppSandboxVDD.inf\" Root\\AppSandboxVDD", dir);
    if (run_devcon(cmd, output, sizeof(output), &update_code, DEVCON_UPDATE_TIMEOUT_MS)) {
        if (update_code == DEVCON_TIMED_OUT) {
            agent_log("VDD update: devcon did not finish within %lu s: %.400s",
                      DEVCON_UPDATE_TIMEOUT_MS / 1000, output);
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                        "display driver update: devcon did not finish within %lu s and was stopped; "
                        "the VM was not restarted (see C:\\Windows\\AppSandbox\\agent.log).",
                        DEVCON_UPDATE_TIMEOUT_MS / 1000);
        } else {
            agent_log("VDD update: devcon exit %lu: %.400s", update_code, output);
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                        "display driver update: devcon exit %lu: %.400s", update_code, output);
        }
    } else {
        DWORD err = GetLastError();
        update_code = 2;
        agent_log("VDD update: devcon failed to launch (%lu).", err);
        _snprintf_s(note, sizeof(note), _TRUNCATE,
                    "display driver update: devcon failed to launch (%lu)", err);
    }
    if (run_devcon(L"enable Root\\AppSandboxVDD", output, sizeof(output), &exit_code,
                   DEVCON_CHANGE_TIMEOUT_MS)) {
        if (exit_code == DEVCON_TIMED_OUT)
            agent_log("VDD update: enable timed out: %.200s", output);
        else
            agent_log("VDD update: enable (exit %lu): %.200s", exit_code, output);
    }
    /* Restart only when devcon itself said so (exit 1); a timeout is not a
       restart request. */
    if (update_code == 1) {
        wchar_t restart[MAX_PATH + 96];
        _snprintf_s(note, sizeof(note), _TRUNCATE,
                    "display driver update installed; restarting the VM in 15 seconds to finish it.");
        agent_log("VDD update: restart required, restarting in 15 s.");
        swprintf_s(restart, ARRAYSIZE(restart),
                   L"\"%s\\shutdown.exe\" /r /t 15 /d p:2:4 /c \"App Sandbox: finishing the display driver update\"",
                   sys_dir);
        run_logged(restart, "VDD update: restart");
    } else if (update_code == 0) {
        _snprintf_s(note, sizeof(note), _TRUNCATE,
                    "display driver updated.");
    }
    if (note[0])
        device_note("%s", note);
}

/* ---- Controllers: ViGEmBus (virtual Xbox 360 pads) ----
 * The guest components carry the ViGEmBus installer (BSD-3, by Nefarius, the
 * bus Sunshine uses). Install it once, in the background, when the bus
 * service is not there yet. */
static BOOL service_exists(const wchar_t *name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc;
    if (!scm) return FALSE;
    svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return svc != NULL;
}

static DWORD WINAPI vigem_install_thread(LPVOID param)
{
    wchar_t *setup = (wchar_t *)param;
    wchar_t cmd[MAX_PATH + 64];
    BOOL ok;
    agent_log("Controllers: installing ViGEmBus...");
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\" /quiet /norestart", setup);
    run_logged(cmd, "Controllers: ViGEmBus setup");
    ok = service_exists(L"ViGEmBus");
    agent_log("Controllers: ViGEmBus %s.", ok ? "installed" : "did not install");
    agent_log_to_host(ok ? "Controllers: ViGEmBus installed in the VM; game controllers can be used."
                         : "Controllers: ViGEmBus could not be installed in the VM (controllers will not work).");
    free(setup);
    return 0;
}

static void ensure_vigem_installed(void)
{
    wchar_t dir[MAX_PATH], *setup, *slash;
    HANDLE t;
    if (service_exists(L"ViGEmBus")) return;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0';
    setup = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
    if (!setup) return;
    swprintf_s(setup, MAX_PATH, L"%s\\drivers\\ViGEmBus_setup.exe", dir);
    if (GetFileAttributesW(setup) == INVALID_FILE_ATTRIBUTES) { free(setup); return; }
    t = CreateThread(NULL, 0, vigem_install_thread, setup, 0, NULL);
    if (t) CloseHandle(t); else free(setup);
}

/* Bring the VDD back when it is disabled or stuck. One bad reading is not
   enough: while PnP is busy (boot, driver installs, logon) devcon can briefly
   report the device stopped or with a problem, and restarting the display
   driver then drops the host's display connection for nothing. So act only
   when two consecutive checks, at least VDD_RECHECK_MIN_MS apart, saw
   "disabled" (enable it) or a problem / stopped device (restart it).
   Anything else -- an unrecognised, empty or timed-out status -- is left
   alone.
   Repairs back off: attempt N+1 waits VDD_REPAIR_BACKOFF_MS[N] after
   attempt N, and after VDD_REPAIR_MAX_ATTEMPTS the agent stops (a problem
   that enable or restart cannot fix, e.g. a signature or IddCx init failure,
   would otherwise restart the driver forever) and says so once. The count
   starts again once the driver has been running for VDD_REPAIR_RESET_MS
   since the last attempt.
   st/status: the reading from read_vdd_state. Returns TRUE when it ran
   devcon enable or restart. Runs only on the device-check thread. */
#define VDD_RECHECK_MIN_MS      10000ULL
#define VDD_REPAIR_MAX_ATTEMPTS 3
#define VDD_REPAIR_RESET_MS     (10ULL * 60 * 1000)
static const ULONGLONG VDD_REPAIR_BACKOFF_MS[VDD_REPAIR_MAX_ATTEMPTS] = { 0, 120000, 300000 };

static BOOL ensure_vdd_running(VddState st, const char *status)
{
    static int       bad_readings = 0;     /* consecutive disabled / problem readings */
    static ULONGLONG first_bad_tick = 0;
    static int       attempts = 0;         /* repairs since the driver last ran for a while */
    static ULONGLONG last_attempt_tick = 0;
    static BOOL      wait_logged = FALSE;
    static BOOL      gave_up = FALSE;
    char output[1024];
    DWORD exit_code = 0;
    ULONGLONG now = GetTickCount64();
    BOOL disabled = st == VDD_DISABLED;

    switch (st) {
    case VDD_RUNNING:
        bad_readings = 0;
        wait_logged = FALSE;
        if (attempts && now - last_attempt_tick >= VDD_REPAIR_RESET_MS) {
            attempts = 0;
            gave_up = FALSE;
        }
        return FALSE;
    case VDD_NOT_FOUND:     /* not installed yet */
        bad_readings = 0;
        return FALSE;
    case VDD_DISABLED:
    case VDD_PROBLEM:
    case VDD_STOPPED:
        break;
    case VDD_UNRECOGNISED:
        agent_log("VDD: unrecognised status - leaving it alone. Output: %.400s", status);
        bad_readings = 0;
        return FALSE;
    default:                /* no reading (devcon missing or timed out) */
        return FALSE;
    }

    /* Problem 14 (CM_PROB_NEED_RESTART): only a restart of Windows helps,
       restarting the device does not. */
    {
        const char *problem = strstr(status, "problem: ");
        if (problem && atoi(problem + 9) == 14) {
            static BOOL restart_logged = FALSE;
            if (!restart_logged) {
                restart_logged = TRUE;
                agent_log("VDD: waits for a restart of Windows (problem 14) - not restarting the device.");
            }
            bad_readings = 0;
            return FALSE;
        }
    }

    if (bad_readings == 0)
        first_bad_tick = now;
    bad_readings++;
    if (bad_readings < 2 || now - first_bad_tick < VDD_RECHECK_MIN_MS) {
        agent_log("VDD: %s - checking again before acting.", disabled ? "disabled" : "problem");
        return FALSE;
    }

    if (attempts >= VDD_REPAIR_MAX_ATTEMPTS) {
        if (!gave_up) {
            gave_up = TRUE;
            agent_log("VDD: still %s after %d repair attempts - not trying again until it has "
                      "run for a while or Windows restarts. Status: %.400s",
                      disabled ? "disabled" : "not working", attempts, status);
            device_note("The display driver (AppSandboxVDD) in this VM still does not work after %d "
                        "repair attempts; restart the VM, and if that does not help, update the guest "
                        "components (see C:\\Windows\\AppSandbox\\agent.log).", attempts);
        }
        return FALSE;
    }
    if (attempts > 0 && now - last_attempt_tick < VDD_REPAIR_BACKOFF_MS[attempts]) {
        if (!wait_logged) {
            wait_logged = TRUE;
            agent_log("VDD: %s again - waiting %llu s after the last repair before trying again.",
                      disabled ? "disabled" : "problem",
                      (VDD_REPAIR_BACKOFF_MS[attempts] - (now - last_attempt_tick)) / 1000);
        }
        return FALSE;
    }

    bad_readings = 0;
    wait_logged = FALSE;
    attempts++;
    last_attempt_tick = now;

    /* A device in CM_PROB_DISABLED (explicitly disabled) has its CONFIGFLAG_DISABLED bit set;
       `devcon restart` (DICS_PROPCHANGE: stop->start) reports success but CANNOT clear that bit,
       so the device snaps straight back to disabled. Only `devcon enable` (DICS_ENABLE) clears it.
       So: ENABLE a disabled device; RESTART a stuck-but-enabled ("problem") one. */
    if (disabled) {
        agent_log("VDD: disabled - enabling (attempt %d of %d).", attempts, VDD_REPAIR_MAX_ATTEMPTS);
        if (!run_devcon(L"enable Root\\AppSandboxVDD", output, sizeof(output), &exit_code,
                        DEVCON_CHANGE_TIMEOUT_MS)) {
            agent_log("VDD enable: devcon failed to launch (%lu).", GetLastError());
            return FALSE;
        }
        if (exit_code == DEVCON_TIMED_OUT)
            agent_log("VDD enable: devcon timed out: %.400s", output);
        else
            agent_log("VDD enable (exit=%lu): %.400s", exit_code, output);
        return TRUE;
    }

    agent_log("VDD: problem - restarting (attempt %d of %d). Status: %.400s",
              attempts, VDD_REPAIR_MAX_ATTEMPTS, status);
    if (!run_devcon(L"restart Root\\AppSandboxVDD", output, sizeof(output), &exit_code,
                    DEVCON_CHANGE_TIMEOUT_MS)) {
        agent_log("VDD restart: devcon failed to launch (%lu).", GetLastError());
        return FALSE;
    }
    if (exit_code == DEVCON_TIMED_OUT)
        agent_log("VDD restart: devcon timed out: %.400s", output);
    else
        agent_log("VDD restart (exit=%lu): %.400s", exit_code, output);
    return TRUE;
}

/* Reads the VDD state once, repairs it if needed and sends idd_status:
   forced first when a host just connected, then (or periodically) only on
   a change, after any repair. Device-check thread only. */
static void check_vdd(BOOL host_just_connected)
{
    char output[4096];
    VddState st = read_vdd_state(output, sizeof(output));
    BOOL repaired;

    if (host_just_connected) {
        report_idd_status(st, output, 1);
        send_device_notes();
    }
    repaired = ensure_vdd_running(st, output);
    if (repaired)
        st = read_vdd_state(output, sizeof(output));
    if (repaired || !host_just_connected)
        report_idd_status(st, output, 0);
}

/* ---- Hyper-V Video device check: disable if running ---- */

#define HYPERV_VIDEO_HWID L"*DA0A7802*"

static void ensure_hyperv_video_disabled(BOOL notify_host)
{
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code,
                    DEVCON_QUERY_TIMEOUT_MS)) {
        agent_log("ensure_hyperv_video_disabled: devcon failed to launch (%lu).", GetLastError());
        return;
    }
    if (exit_code == DEVCON_TIMED_OUT) {
        agent_log("ensure_hyperv_video_disabled: devcon status timed out.");
        return;
    }

    if (strstr(output, "disabled") || strstr(output, "No matching"))
        return;

    if (!strstr(output, "running"))
        return;

    agent_log("Hyper-V Video: running — disabling.");

    if (!run_devcon(L"disable " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code,
                    DEVCON_CHANGE_TIMEOUT_MS)) {
        agent_log("Hyper-V Video disable: devcon failed to launch (%lu).", GetLastError());
        return;
    }
    if (exit_code == DEVCON_TIMED_OUT) {
        agent_log("Hyper-V Video disable: devcon timed out: %.400s", output);
        return;
    }

    if (notify_host)
        send_to_host("hyperv_video:disabled");
    agent_log("Hyper-V Video disable (exit=%lu): %.400s", exit_code, output);
}

/* ---- Report display monitor info to host ---- */

/* Spawn appsandbox-displays.exe in the interactive session and capture its
   stdout.  The helper prints "N,WxH,WxH,..." and exits.  We use the same
   token-dup + SetTokenInformation pattern as spawn_input_in_session(). */
static void report_displays(AsbConn *notify)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    DWORD session_id;
    char output[512];
    DWORD bytes_read;
    char msg[512];

    output[0] = '\0';

    session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF || session_id == 0) {
        /* No interactive user session — skip display enumeration.
           Session 0 is the services session (no desktop).
           0xFFFFFFFF means no console session at all. */
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    /* Build path to appsandbox-displays.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-displays.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("report_displays: %ls not found.", exe_path);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    /* Create pipe for stdout capture */
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        agent_log("report_displays: CreatePipe failed (%lu).", GetLastError());
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Duplicate our token and set the interactive session */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("report_displays: OpenProcessToken failed (%lu).", GetLastError());
        CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("report_displays: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("report_displays: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("report_displays: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != NULL) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(dup_token);
    CloseHandle(hWritePipe);  /* Close write end so ReadFile sees EOF */
    hWritePipe = NULL;

    /* Wait for process (max 2s) then read output.
       Timeout is normal at early boot before the desktop is ready. */
    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
        agent_log("report_displays: helper timed out (desktop may not be ready).");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    {
        int total = 0;
        while (ReadFile(hReadPipe, output + total,
                        (DWORD)(sizeof(output) - 1 - total), &bytes_read, NULL) &&
               bytes_read > 0) {
            total += (int)bytes_read;
            if (total >= (int)sizeof(output) - 1) break;
        }
        output[total] = '\0';
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    /* Strip trailing whitespace */
    {
        int len = (int)strlen(output);
        while (len > 0 && (output[len-1] == '\r' || output[len-1] == '\n' || output[len-1] == ' '))
            output[--len] = '\0';
    }

    snprintf(msg, sizeof(msg), "displays:%s", output);
    agent_log("Displays: %s", output);
    if (notify != NULL) send_line(notify, msg);
}

/* ---- IDD connect: respawn helpers in console session ---- */

/* Send a response, prepending the sequence tag if present */
static void send_reply(AsbConn *s, const char *tag, const char *msg)
{
    if (tag[0]) {
        char rb[512];
        sprintf_s(rb, sizeof(rb), "%s%s", tag, msg);
        send_line(s, rb);
    } else {
        send_line(s, msg);
    }
}

static void handle_idd_connect(AsbConn *client, const char *tag)
{
    DWORD new_console = WTSGetActiveConsoleSessionId();

    /* Kill and respawn input helper in the console session */
    kill_input_helper();
    agent_log("idd_connect: respawning input helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_input_in_session(new_console);

    /* Kill and respawn clipboard helper (SYSTEM, :0005) in the console session */
    kill_clipboard_helper();
    agent_log("idd_connect: respawning clipboard helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_in_session(new_console);

    /* Kill and respawn clipboard reader (user, :0006) in the console session */
    kill_clipboard_reader();
    agent_log("idd_connect: respawning clipboard reader in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_reader_in_session(new_console);

    send_reply(client, tag, "ok");
}

/* ---- Persistent client handler ---- */

/* Disable the Hyper-V synthetic video adapter (if present).
   Hardware ID: VMBUS\{da0a7802-e377-4aac-8e77-0558eb1073f8}
   Silently does nothing if the device doesn't exist. */
static void disable_hyperv_video(BOOL notify_host)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    wchar_t hw_id[512];
    CONFIGRET cr;
    static const wchar_t *TARGET_HWID = L"VMBUS\\{DA0A7802-E377-4AAC-8E77-0558EB1073F8}";

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("disable_hyperv_video: SetupDiGetClassDevs(PRESENT) failed, trying ALLCLASSES.");
        devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
        if (devs == INVALID_HANDLE_VALUE) return;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        ULONG dev_status = 0, dev_problem = 0;
        hw_id[0] = L'\0';
        CM_Get_Device_IDW(dev_info.DevInst, hw_id, 512, 0);

        if (_wcsnicmp(hw_id, TARGET_HWID, wcslen(TARGET_HWID)) != 0)
            continue;

        /* Check if already disabled */
        cr = CM_Get_DevNode_Status(&dev_status, &dev_problem, dev_info.DevInst, 0);
        if (cr == CR_SUCCESS && !(dev_status & DN_STARTED) && (dev_problem == CM_PROB_DISABLED)) {
            agent_log("Hyper-V Video adapter already disabled: %ls", hw_id);
            if (notify_host)
                send_to_host("hyperv_video:already_disabled");
            SetupDiDestroyDeviceInfoList(devs);
            return;
        }

        agent_log("Disabling Hyper-V Video adapter: %ls (status=0x%lX problem=%lu)", hw_id, dev_status, dev_problem);
        if (notify_host)
            send_to_host("hyperv_video:disabling");

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr == CR_SUCCESS) {
            agent_log("Hyper-V Video adapter disabled.");
            if (notify_host)
                send_to_host("hyperv_video:disabled");
        } else {
            agent_log("Hyper-V Video disable failed (%lu).", cr);
        }
        SetupDiDestroyDeviceInfoList(devs);
        return;
    }

    agent_log("Hyper-V Video adapter not found (enumerated %lu devices).", idx);
    if (notify_host)
        send_to_host("hyperv_video:not_found");
    SetupDiDestroyDeviceInfoList(devs);
}

/* Forward declaration — defined after SSH proxy section */
static void handle_ssh_enable(AsbConn *client, const char *tag);

static ULONG primary_nic_index(void)
{
    ULONG buf_len = 15000;
    ULONG index = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        IP_ADAPTER_ADDRESSES *addrs, *cur;
        ULONG ret;

        addrs = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs) return 0;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, addrs, &buf_len);
        if (ret == NO_ERROR) {
            for (cur = addrs; cur; cur = cur->Next) {
                if (cur->IfType != IF_TYPE_ETHERNET_CSMACD || !cur->IfIndex)
                    continue;
                if (!index) index = cur->IfIndex;
                if (cur->OperStatus == IfOperStatusUp) {
                    index = cur->IfIndex;
                    break;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, addrs);
        if (ret != ERROR_BUFFER_OVERFLOW) break;
    }
    return index;
}

/* ---- Diagnostics: did the previous Windows session end cleanly? ----
 * CleanShutdown (DWORD under HKLM\SOFTWARE\AppSandbox\Agent) is 0 while the
 * service runs and 1 once it is told to stop or that Windows shuts down.
 * Finding 0 at the first service start of a boot means the last session
 * ended some other way: a crash (bugcheck), a reset, a power-off, or Force
 * Stop in App Sandbox, the usual way to end a disposable VM. So the host log
 * is told only when there is evidence of a crash: the bugcheck Windows
 * recorded in this boot's System log (WER-SystemErrorReporting 1001, or
 * Kernel-Power 41 with a bugcheck code) or a crash dump not reported before.
 * Without it, only agent.log gets a line. Windows writes the bugcheck event
 * and extracts the dump a while after boot, so the report is put together at
 * the first host connection and sent once. Best effort: any registry, event
 * log or file error just leaves that part out. */

static BOOL      g_prev_unclean;        /* CleanShutdown was 0 at the first start in this boot */
static ULONGLONG g_last_dump_reported;  /* LastMinidumpReported at service start */
/* Used only by the connection thread (handle_client): */
static BOOL      g_session_report_built;
static char      g_reset_note[900];     /* built, not sent yet */
static ULONGLONG g_reset_dump_time;     /* write time of the dump named in g_reset_note */

static void mark_clean_shutdown(DWORD clean)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"CleanShutdown", 0, REG_DWORD, (const BYTE *)&clean, sizeof(clean));
    /* Write it through now: a crash or reset later in this session must find
       the 0, and a shutdown must not lose the 1. */
    RegFlushKey(key);
    RegCloseKey(key);
}

/* Newest *.dmp in %windir%\Minidump written after 'after' (a FILETIME as a
   64-bit value). Returns FALSE when there is none. */
static BOOL newest_minidump(ULONGLONG after, wchar_t *name, int name_len, ULONGLONG *written)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    UINT len = GetWindowsDirectoryW(pattern, MAX_PATH);
    BOOL found = FALSE;

    if (!len || len >= MAX_PATH - 20) return FALSE;
    wcscat_s(pattern, MAX_PATH, L"\\Minidump\\*.dmp");
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        ULONGLONG t;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t > after && (!found || t > *written)) {
            *written = t;
            wcscpy_s(name, name_len, fd.cFileName);
            found = TRUE;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

/* At service start: read the previous session's marker, set it to 0 for
   this one, and note in agent.log how the previous session ended. */
static void check_previous_session(void)
{
    static volatile LONG started_flag = 0;
    HKEY key;
    DWORD value = 1, type = 0, size = sizeof(value);
    ULONGLONG last_dump = 0;
    /* A restart of this service within the same boot (it crashed and the SCM
       restarted it) is not a Windows session ending. */
    BOOL first_start = first_time_this_boot(L"StartedThisBoot", &started_flag);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExW(key, L"CleanShutdown", NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value) && value == 0)
        g_prev_unclean = first_start;
    size = sizeof(last_dump);
    if (RegQueryValueExW(key, L"LastMinidumpReported", NULL, &type, (BYTE *)&last_dump, &size) != ERROR_SUCCESS ||
        type != REG_QWORD || size != sizeof(last_dump))
        last_dump = 0;
    RegCloseKey(key);
    g_last_dump_reported = last_dump;
    mark_clean_shutdown(0);

    if (g_prev_unclean)
        agent_log("The previous Windows session did not shut down normally (CleanShutdown was 0): "
                  "a crash, a reset, a power-off or Force Stop. Crash details, if any, are reported "
                  "at the first host connection.");
}

/* Text of the n-th (0-based) <Data> element of an event's XML, or of the
   <Data> element named 'name' when name is not NULL. Entities are not
   decoded. FALSE when there is no such element. */
static BOOL event_data(const wchar_t *xml, int n, const wchar_t *name, wchar_t *out, size_t out_len)
{
    const wchar_t *p = xml, *end;
    size_t len;

    for (;;) {
        p = wcsstr(p, L"<Data");
        if (!p) return FALSE;
        end = wcschr(p, L'>');
        if (!end) return FALSE;
        if (name) {
            /* Name='x' or Name="x" inside this start tag */
            const wchar_t *attr = wcsstr(p, L"Name=");
            if (attr && attr < end && (attr[5] == L'\'' || attr[5] == L'"') &&
                wcsncmp(attr + 6, name, wcslen(name)) == 0 && attr[6 + wcslen(name)] == attr[5])
                break;
        } else if (n-- == 0) {
            break;
        }
        p = end;
    }
    out[0] = L'\0';
    if (end[-1] == L'/') return TRUE;   /* <Data .../>: empty */
    p = end + 1;
    end = wcschr(p, L'<');
    if (!end) return FALSE;
    len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    wmemcpy(out, p, len);
    out[len] = L'\0';
    return TRUE;
}

/* XML of the newest System-log event from 'provider' with 'event_id' logged
   in the last within_ms milliseconds, or NULL (free() it). */
static wchar_t *newest_system_event(const wchar_t *provider, int event_id, ULONGLONG within_ms)
{
    wchar_t query[300];
    EVT_HANDLE results, event = NULL;
    DWORD returned = 0, used = 0, props = 0;
    wchar_t *xml = NULL;

    swprintf_s(query, ARRAYSIZE(query),
               L"*[System[Provider[@Name='%s'] and EventID=%d and "
               L"TimeCreated[timediff(@SystemTime) <= %llu]]]",
               provider, event_id, within_ms);
    results = EvtQuery(NULL, L"System", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) return NULL;
    if (EvtNext(results, 1, &event, 2000, 0, &returned) && returned == 1) {
        if (!EvtRender(NULL, event, EvtRenderEventXml, 0, NULL, &used, &props) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && used) {
            xml = (wchar_t *)calloc(used / sizeof(wchar_t) + 1, sizeof(wchar_t));
            if (xml && !EvtRender(NULL, event, EvtRenderEventXml, used, xml, &used, &props)) {
                free(xml);
                xml = NULL;
            }
        }
        EvtClose(event);
    }
    EvtClose(results);
    return xml;
}

/* The bugcheck Windows recorded in this boot (so for the previous session):
   text as "0x0000003b (0x..., 0x..., 0x..., 0x...)", the code and first
   parameter, and the dump file WER named (may be empty). */
typedef struct {
    char      text[200];
    ULONGLONG code, param1;
    wchar_t   dump[MAX_PATH];
} BugcheckInfo;

static BOOL recorded_bugcheck(BugcheckInfo *bc)
{
    /* Events since this boot, plus a margin for the clock being set at boot. */
    ULONGLONG within = GetTickCount64() + 120000;
    wchar_t value[200], *xml;
    BOOL found = FALSE;

    ZeroMemory(bc, sizeof(*bc));
    /* "The computer has rebooted from a bugcheck": param1 is the bugcheck
       text, param2 the dump file. */
    xml = newest_system_event(L"Microsoft-Windows-WER-SystemErrorReporting", 1001, within);
    if (xml && event_data(xml, 0, NULL, value, ARRAYSIZE(value)) && value[0]) {
        const wchar_t *open = wcschr(value, L'(');
        bc->code = wcstoull(value, NULL, 0);
        bc->param1 = open ? wcstoull(open + 1, NULL, 0) : 0;
        _snprintf_s(bc->text, sizeof(bc->text), _TRUNCATE, "%ls", value);
        if (!event_data(xml, 1, NULL, bc->dump, ARRAYSIZE(bc->dump)))
            bc->dump[0] = L'\0';
        found = bc->code != 0;
    }
    free(xml);
    if (found) return TRUE;

    /* Kernel-Power 41 ("rebooted without cleanly shutting down") carries the
       bugcheck code when there was one, 0 otherwise. */
    xml = newest_system_event(L"Microsoft-Windows-Kernel-Power", 41, within);
    if (xml && event_data(xml, 0, L"BugcheckCode", value, ARRAYSIZE(value)) &&
        (bc->code = wcstoull(value, NULL, 0)) != 0) {
        ULONGLONG p[4] = { 0, 0, 0, 0 };
        static const wchar_t *const names[4] = {
            L"BugcheckParameter1", L"BugcheckParameter2", L"BugcheckParameter3", L"BugcheckParameter4"
        };
        int k;
        for (k = 0; k < 4; k++)
            if (event_data(xml, 0, names[k], value, ARRAYSIZE(value)))
                p[k] = wcstoull(value, NULL, 0);
        bc->param1 = p[0];
        _snprintf_s(bc->text, sizeof(bc->text), _TRUNCATE,
                    "0x%08llX (0x%llX, 0x%llX, 0x%llX, 0x%llX)", bc->code, p[0], p[1], p[2], p[3]);
        found = TRUE;
    }
    free(xml);
    return found;
}

static BOOL windows_build(DWORD *build, DWORD *ubr);
static int gpu_crash_build_status(DWORD build, DWORD ubr);

/* Puts the host-log note together (see above). Connection thread only. */
static void build_session_report(void)
{
    static volatile LONG crash_reported_flag = 0;
    BugcheckInfo bc = { 0 };
    wchar_t dump[MAX_PATH];
    ULONGLONG dump_time = 0;
    BOOL have_dump = newest_minidump(g_last_dump_reported, dump, MAX_PATH, &dump_time);
    BOOL have_bugcheck = (g_prev_unclean || have_dump) && recorded_bugcheck(&bc);
    char *c;

    if (have_bugcheck && first_time_this_boot(L"CrashReportedThisBoot", &crash_reported_flag)) {
        DWORD build = 0, ubr = 0;
        /* SYSTEM_SERVICE_EXCEPTION with an access violation, on a build with
           the GPU-PV dxgkrnl bug (see gpu_crash_workaround). */
        BOOL known = bc.code == 0x3B && (bc.param1 & 0xFFFFFFFFULL) == 0xC0000005ULL &&
                     (!windows_build(&build, &ubr) || gpu_crash_build_status(build, ubr) == 1);

        agent_log("Previous Windows session ended with bugcheck %s.%s%ls", bc.text,
                  have_dump ? " Crash dump: " : "", have_dump ? dump : L"");
        _snprintf_s(g_reset_note, sizeof(g_reset_note), _TRUNCATE,
                    "log:Windows in this VM crashed in its previous session (bugcheck %s)%s%ls%ls%s%s",
                    bc.text,
                    have_dump || bc.dump[0] ? "; crash dump: " : "",
                    have_dump ? L"C:\\Windows\\Minidump\\" : L"",
                    have_dump ? dump : bc.dump,
                    known ? ". This matches a known Windows bug in VMs with a GPU (dxgkrnl, GPU-PV); "
                            "installing the latest Windows update in the VM fixes it" : "",
                    known ? "." : "");
    } else if (have_dump) {
        agent_log("New crash dump found: %ls", dump);
        _snprintf_s(g_reset_note, sizeof(g_reset_note), _TRUNCATE,
                    "log:Windows wrote a crash dump in this VM: C:\\Windows\\Minidump\\%ls "
                    "(see the System event log)", dump);
    } else if (g_prev_unclean) {
        agent_log("No crash was recorded for the previous Windows session (Force Stop, power-off or "
                  "reset); nothing reported to the host.");
    }
    for (c = g_reset_note; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    if (have_dump)
        g_reset_dump_time = dump_time;
}

/* Send the note once, on the first connection that takes it, and remember
   the dump as reported. */
static void report_previous_session(AsbConn *client)
{
    HKEY key;

    if (!g_session_report_built) {
        g_session_report_built = TRUE;
        build_session_report();
    }
    if (!g_reset_note[0] || send_line(client, g_reset_note) <= 0)
        return;
    g_reset_note[0] = '\0';
    if (g_reset_dump_time &&
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"LastMinidumpReported", 0, REG_QWORD,
                       (const BYTE *)&g_reset_dump_time, sizeof(g_reset_dump_time));
        RegCloseKey(key);
    }
}

/* ---- GPU crash workaround (Windows feature overrides) ----
 * Windows 11 builds 26100, 26200 and 26300 from UBR 9550 (KB5124010,
 * September 2026; KB5127648 brought it to all three) have a dxgkrnl bug in
 * GPU-PV guests: asking every display adapter about video-encoder or
 * exclusive-engine support also reaches the Microsoft Basic Render (WARP)
 * adapter, which has no VMBus channel, and the guest bugchecks with 0x3B
 * (c0000005 in DXGVMBUSMESSAGE::InitializeMessage) and restarts. Chromium
 * apps (Edge, Discord, Steam's web views) and games trigger it through
 * MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER), and Task Manager, dxdiag and the
 * Game DVR service do too. KB5127936 / the November 10 2026 update (26200 and
 * 26300 UBR 9818) fixes it; 26100 Home/Pro gets no more updates.
 *
 * The workaround turns off the two features that make those queries, with
 * boot-store feature overrides (priority 8 = User), the same keys ViVeTool
 * writes for "/disable /id:56095990,62328181":
 *   56095990  Feature_Servicing_MFTEnumEngineAccessFilter (mfplat)
 *   62328181  Feature_D3DVideoEncoderExclusiveAccess (D3D12Core)
 * Override key names are obfuscated IDs, computed with ViVeTool's
 * ObfuscateFeatureId: rotl1(bswap(id ^ 0x74161A4E) ^ 0x8FB23D4F) ^ 0x833EA8FF.
 * Windows reads them at boot, so they take effect after the next restart.
 *
 * Applied only to an affected build with a GPU-PV adapter (or GPU shares from
 * the host). The keys this agent created are listed in FeatureOverridesSet
 * (AGENT_REG_KEY) and are the only ones it ever changes or removes: an
 * existing override key it did not create is left alone. They are removed
 * again on a fixed or unaffected build, and when GpuStabilityTweaks (DWORD,
 * AGENT_REG_KEY) is 0, which turns the workaround off. Device-check thread
 * only. */
#define FEATURE_OVERRIDES_KEY L"SYSTEM\\CurrentControlSet\\Control\\FeatureManagement\\Overrides\\8"

static const struct {
    const wchar_t *key;        /* obfuscated ID: the override's key name */
    DWORD          feature;    /* feature ID as ViVeTool shows it */
} GPU_CRASH_FEATURES[] = {
    { L"3985002639", 56095990 },   /* Feature_Servicing_MFTEnumEngineAccessFilter */
    { L"3933573262", 62328181 },   /* Feature_D3DVideoEncoderExclusiveAccess */
};
#define GPU_CRASH_FEATURE_COUNT (sizeof(GPU_CRASH_FEATURES) / sizeof(GPU_CRASH_FEATURES[0]))

/* TRUE when a display adapter driven by vrd.inf (GPU-PV) is present. */
static BOOL gpu_pv_adapter_present(void)
{
    HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    BOOL found = FALSE;

    if (devs == INVALID_HANDLE_VALUE) return FALSE;
    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; !found && SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        if (SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath, &prop_type,
                                      (PBYTE)inf_path, sizeof(inf_path), NULL, 0) &&
            _wcsicmp(inf_path, L"vrd.inf") == 0)
            found = TRUE;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

/* CurrentBuild and UBR of this Windows; FALSE when they cannot be read. */
static BOOL windows_build(DWORD *build, DWORD *ubr)
{
    wchar_t text[16];
    DWORD size = sizeof(text);

    *build = 0;
    *ubr = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuild", RRF_RT_REG_SZ, NULL, text, &size) != ERROR_SUCCESS)
        return FALSE;
    *build = (DWORD)wcstoul(text, NULL, 10);
    size = sizeof(*ubr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"UBR", RRF_RT_REG_DWORD, NULL, ubr, &size) != ERROR_SUCCESS)
        *ubr = 0;
    return *build != 0;
}

/* 1 when this Windows build has the GPU-PV crash bug, 2 when it is a build
   of an affected release that has the fix, 0 otherwise. */
static int gpu_crash_build_status(DWORD build, DWORD ubr)
{
    if ((build == 26200 || build == 26300) && ubr >= 9818) return 2;
    if ((build == 26100 || build == 26200 || build == 26300) && ubr >= 9550) return 1;
    return 0;
}

/* Which of GPU_CRASH_FEATURES this agent created (FeatureOverridesSet). */
static void read_overrides_set(BOOL ours[GPU_CRASH_FEATURE_COUNT])
{
    wchar_t list[256];
    DWORD size = sizeof(list) - 2 * sizeof(wchar_t);
    const wchar_t *name;
    size_t f;

    for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++) ours[f] = FALSE;
    ZeroMemory(list, sizeof(list));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, L"FeatureOverridesSet",
                     RRF_RT_REG_MULTI_SZ, NULL, list, &size) != ERROR_SUCCESS)
        return;
    for (name = list; *name; name += wcslen(name) + 1)
        for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++)
            if (wcscmp(name, GPU_CRASH_FEATURES[f].key) == 0) ours[f] = TRUE;
}

static BOOL write_overrides_set(const BOOL ours[GPU_CRASH_FEATURE_COUNT])
{
    wchar_t list[256];
    size_t at = 0, f;
    HKEY key;
    LSTATUS rc;

    ZeroMemory(list, sizeof(list));
    for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++) {
        if (!ours[f]) continue;
        wcscpy_s(list + at, ARRAYSIZE(list) - at - 1, GPU_CRASH_FEATURES[f].key);
        at += wcslen(GPU_CRASH_FEATURES[f].key) + 1;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;
    if (at)
        rc = RegSetValueExW(key, L"FeatureOverridesSet", 0, REG_MULTI_SZ, (const BYTE *)list,
                            (DWORD)((at + 1) * sizeof(wchar_t)));
    else {
        rc = RegDeleteValueW(key, L"FeatureOverridesSet");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegFlushKey(key);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

/* Creates the override key for feature f if it does not exist yet. Returns
   1 when this call created it, 0 when it exists already (not touched), -1 on
   an error. The key is listed in FeatureOverridesSet before it is created, so
   a crash in between cannot leave an override nobody removes. */
static int create_feature_override(size_t f, BOOL ours[GPU_CRASH_FEATURE_COUNT])
{
    static const wchar_t *const zero_values[] = {
        L"EnabledStateOptions", L"Variant", L"VariantPayload", L"VariantPayloadKind"
    };
    wchar_t path[160];
    HKEY key;
    DWORD disposition = 0, disabled = 1, zero = 0, i;
    LSTATUS rc;
    BOOL was_ours = ours[f];

    swprintf_s(path, ARRAYSIZE(path), L"%s\\%s", FEATURE_OVERRIDES_KEY, GPU_CRASH_FEATURES[f].key);
    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key);
    if (rc == ERROR_SUCCESS) {
        RegCloseKey(key);
        return 0;
    }
    if (rc != ERROR_FILE_NOT_FOUND)
        return -1;
    if (!was_ours) {
        ours[f] = TRUE;
        if (!write_overrides_set(ours)) {
            ours[f] = FALSE;
            return -1;
        }
    }
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key,
                         &disposition);
    if (rc != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        if (rc == ERROR_SUCCESS) RegCloseKey(key);
        if (!was_ours) {
            ours[f] = FALSE;
            write_overrides_set(ours);
        }
        if (rc != ERROR_SUCCESS)
            agent_log("GPU crash workaround: cannot create the override for feature %lu (error %ld).",
                      GPU_CRASH_FEATURES[f].feature, (long)rc);
        return rc == ERROR_SUCCESS ? 0 : -1;
    }
    /* EnabledState 1 = disabled (0 default, 2 enabled). */
    rc = RegSetValueExW(key, L"EnabledState", 0, REG_DWORD, (const BYTE *)&disabled, sizeof(disabled));
    for (i = 0; rc == ERROR_SUCCESS && i < ARRAYSIZE(zero_values); i++)
        rc = RegSetValueExW(key, zero_values[i], 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
    RegFlushKey(key);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        agent_log("GPU crash workaround: cannot write the override for feature %lu (error %ld).",
                  GPU_CRASH_FEATURES[f].feature, (long)rc);
        return -1;
    }
    return 1;
}

/* Removes the override for feature f that this agent created, unless its
   state was changed since (then it now belongs to whoever changed it).
   Either way it is no longer listed as ours. Returns TRUE when removed. */
static BOOL remove_feature_override(size_t f, BOOL ours[GPU_CRASH_FEATURE_COUNT])
{
    wchar_t path[160];
    DWORD state = 0, size = sizeof(state);
    LSTATUS rc;
    BOOL removed = FALSE, keep = FALSE;

    swprintf_s(path, ARRAYSIZE(path), L"%s\\%s", FEATURE_OVERRIDES_KEY, GPU_CRASH_FEATURES[f].key);
    rc = RegGetValueW(HKEY_LOCAL_MACHINE, path, L"EnabledState", RRF_RT_REG_DWORD, NULL, &state, &size);
    if (rc == ERROR_SUCCESS && state == 1) {
        LSTATUS del = RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
        removed = del == ERROR_SUCCESS;
        keep = !removed;    /* still ours: try again next time */
        if (!removed)
            agent_log("GPU crash workaround: cannot remove the override for feature %lu (error %ld).",
                      GPU_CRASH_FEATURES[f].feature, (long)del);
    } else if (rc == ERROR_SUCCESS) {
        agent_log("GPU crash workaround: the override for feature %lu was changed (state %lu); "
                  "leaving it as it is.", GPU_CRASH_FEATURES[f].feature, state);
    }
    if (!keep) {
        ours[f] = FALSE;
        write_overrides_set(ours);
    }
    return removed;
}

/* Applies or reverts the workaround (see above) and logs the decision when
   it differs from the last one. gpu_shares: the host sent GPU shares. */
static void gpu_crash_workaround(BOOL gpu_shares)
{
    static char last_decision[200];
    char decision[200];
    BOOL ours[GPU_CRASH_FEATURE_COUNT];
    DWORD build = 0, ubr = 0, tweaks = 1, size = sizeof(tweaks);
    BOOL have_build, affected, fixed, gpu, any_ours = FALSE;
    int applied = 0, removed = 0, foreign = 0;
    size_t f;

    read_overrides_set(ours);
    for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++) any_ours |= ours[f];
    if (RegGetValueW(HKEY_LOCAL_MACHINE, AGENT_REG_KEY, L"GpuStabilityTweaks", RRF_RT_REG_DWORD,
                     NULL, &tweaks, &size) != ERROR_SUCCESS)
        tweaks = 1;
    have_build = windows_build(&build, &ubr);
    fixed = gpu_crash_build_status(build, ubr) == 2;
    affected = gpu_crash_build_status(build, ubr) == 1;
    gpu = gpu_shares || gpu_pv_adapter_present();

    if (tweaks == 0 || (have_build && !affected)) {
        for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++)
            if (ours[f] && remove_feature_override(f, ours)) removed++;
        _snprintf_s(decision, sizeof(decision), _TRUNCATE, "%s%s",
                    tweaks == 0 ? "turned off (GpuStabilityTweaks=0)"
                                : fixed ? "not needed (this build has the fix)"
                                        : "not needed (this build is not affected)",
                    any_ours ? "; removing the overrides it set" : "");
    } else if (!have_build) {
        _snprintf_s(decision, sizeof(decision), _TRUNCATE, "Windows build unknown, nothing changed");
    } else if (!gpu) {
        _snprintf_s(decision, sizeof(decision), _TRUNCATE,
                    "affected build, but no GPU-PV adapter; nothing changed");
    } else {
        for (f = 0; f < GPU_CRASH_FEATURE_COUNT; f++) {
            int rc = create_feature_override(f, ours);
            if (rc > 0) applied++;
            else if (rc == 0 && !ours[f]) foreign++;
        }
        _snprintf_s(decision, sizeof(decision), _TRUNCATE,
                    "affected build with GPU-PV: feature overrides %s%s",
                    applied ? "set now (take effect after the next restart)" : "in place",
                    foreign ? "; an override set by someone else was left alone" : "");
    }

    if (strcmp(decision, last_decision) != 0 || applied || removed) {
        agent_log("GPU crash workaround: Windows build %lu.%lu - %s.", build, ubr, decision);
        strcpy_s(last_decision, sizeof(last_decision), decision);
    }
    if (applied)
        device_note("GPU crash workaround applied: Windows %lu.%lu in this VM has a known bug that can "
                    "crash and restart it when apps use the GPU (Edge, Discord, Steam, games). The "
                    "workaround takes effect after the VM restarts; installing the latest Windows update "
                    "in the VM also fixes it.", build, ubr);
    if (removed)
        agent_log("GPU crash workaround: %d override(s) removed; the change takes effect after the "
                  "next restart.", removed);
}

/* ---- Device checks (VDD, Hyper-V Video, idd_status) ----
 * devcon waits for PnP and can take many seconds while PnP is busy (boot,
 * driver installs). The checks therefore run on their own thread and never
 * on the connection thread, which must keep answering host commands: current
 * hosts drop the connection when a tagged reply takes longer than 60 s (older
 * hosts after 5 s without any line). All devcon use is serialized on this
 * thread, including the staged display-driver update at service start. */
#define DEVICE_CHECK_INTERVAL_MS 20000

static HANDLE        g_device_check_wake;       /* auto-reset: run a check now */
static volatile LONG g_device_check_connect;    /* a host connected: full check, forced idd_status */
static volatile LONG g_device_check_logoff;     /* a user logged off: Hyper-V Video check only */
static volatile LONG g_device_check_gpu;        /* the host sent GPU shares: GPU crash workaround */

static void request_device_check(volatile LONG *reason)
{
    InterlockedExchange(reason, 1);
    if (g_device_check_wake) SetEvent(g_device_check_wake);
}

/* The host sent GPU shares: the VM has a GPU (gpu_crash_workaround). */
static void gpu_shares_received(void)
{
    request_device_check(&g_device_check_gpu);
}

/* param != NULL: also run the service-start tasks first. */
static DWORD WINAPI device_check_thread(LPVOID param)
{
    HANDLE waits[2];
    DWORD count = 1;

    if (param) {
        /* Before the display-driver update, which may restart Windows. */
        gpu_crash_workaround(FALSE);

        /* A newer display driver staged by the host while the VM was off. */
        apply_pending_vdd_update();
        ensure_vigem_installed();

        /* Ensure VDD device is running (may need restart after logout teardown) */
        check_vdd(FALSE);

        /* Ensure Hyper-V Video adapter is disabled (VDD replaces it) */
        ensure_hyperv_video_disabled(FALSE);
    }

    waits[0] = g_stop_event;
    if (g_device_check_wake) waits[count++] = g_device_check_wake;

    for (;;) {
        DWORD w = WaitForMultipleObjects(count, waits, FALSE, DEVICE_CHECK_INTERVAL_MS);
        if (w != WAIT_TIMEOUT && w != WAIT_OBJECT_0 + 1) break;   /* stop, or the wait failed */

        if (InterlockedExchange(&g_device_check_connect, 0)) {
            /* New connection: the host resets idd_ready and waits for a status
               (check_vdd sends it first, then the pending notes). */
            check_vdd(TRUE);
            ensure_hyperv_video_disabled(TRUE);
        } else if (w == WAIT_TIMEOUT && host_connected()) {
            /* Periodic check while a host is connected (VDD recovery is expensive) */
            check_vdd(FALSE);
            ensure_hyperv_video_disabled(TRUE);
        }

        if (InterlockedExchange(&g_device_check_gpu, 0))
            gpu_crash_workaround(TRUE);

        if (InterlockedExchange(&g_device_check_logoff, 0)) {
            /* Disable Hyper-V Video after logoff (may have been re-enabled).
               The VDD is deliberately not checked here; see service_ctrl_ex. */
            ensure_hyperv_video_disabled(FALSE);
        }
    }
    return 0;
}

static void handle_client(AsbConn *client)
{
    char buf[256];
    int n;
    DWORD heartbeat_interval = 5000;     /* ms between heartbeats */
    DWORD last_heartbeat;

    agent_log("Client connected.");

    /* Set timeouts so recv/send don't block forever if host disconnects (no-op on ivshmem) */
    asb_set_timeout(client, 10000, 10000);

    /* Send hello, and publish the connection to the other threads in the same
       locked step: the host requires "hello" to be the first line. */
    EnterCriticalSection(&g_send_cs);
    n = send_line(client, "hello");
    if (n > 0)
        g_client_sock = client;
    LeaveCriticalSection(&g_send_cs);
    if (n <= 0) {
        agent_log("Failed to send hello.");
        asb_close(client);
        return;
    }

    report_previous_session(client);

    /* The host clears its display readiness on every connection and opens no
       display until it gets idd_status. Send the device-check thread's last
       reading now: that thread may be busy for minutes (a staged driver
       update, a repair) before it gets to the live, forced status below. */
    {
        const char *cached = recent_idd_status();
        if (cached) {
            char line[32];
            sprintf_s(line, sizeof(line), "idd_status:%s", cached);
            send_line(client, line);
        }
    }

    /* The device-check thread reports the IDD driver status (forced, so a
       reconnect re-syncs), any display-driver update result, and runs the
       initial device checks without holding up this connection. */
    request_device_check(&g_device_check_connect);

    last_heartbeat = GetTickCount();

    /* Persistent connection loop */
    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        int ret;
        DWORD now;

        ret = asb_poll(client, 1000);   /* 1s: 1=data, 0=timeout, <0=closed/error */
        if (ret < 0) break;

        /* Send heartbeat if interval elapsed */
        now = GetTickCount();
        if (now - last_heartbeat >= heartbeat_interval) {
            if (send_line(client, "heartbeat") <= 0) {
                agent_log("Heartbeat send failed, client disconnected.");
                break;
            }
            last_heartbeat = now;
        }


        if (ret == 0) continue; /* select timeout, no data */

        /* Read command */
        n = recv_line(client, buf, sizeof(buf));
        if (n <= 0) {
            agent_log("Client disconnected.");
            break;
        }

        /* Extract sequence tag (e.g. "42:ping" → tag="42:", cmd="ping") */
        {
            char *cmd = buf;
            char tag[32] = {0};
            char *colon = strchr(buf, ':');
            if (colon && colon > buf && colon - buf < 16) {
                /* Check all chars before colon are digits */
                char *p;
                BOOL is_seq = TRUE;
                for (p = buf; p < colon; p++) {
                    if (*p < '0' || *p > '9') { is_seq = FALSE; break; }
                }
                if (is_seq) {
                    int tlen = (int)(colon - buf + 1);
                    memcpy(tag, buf, tlen);
                    tag[tlen] = '\0';
                    cmd = colon + 1;
                }
            }

        agent_log("Command: %s", buf);

        /* Helper macro: send response with tag prefix */
        #define REPLY(msg) send_reply(client, tag, msg)

        if (strcmp(cmd, "ping") == 0) {
            REPLY("ok");
        }
        else if (strncmp(cmd, "ssh_deploy_key ", 15) == 0) {
            REPLY(deploy_ssh_key(cmd + 15) ? "ssh_key_deployed" : "ssh_key_failed");
        }
        else if (strcmp(cmd, "shutdown") == 0) {
            REPLY("ok");
            agent_log("Initiating shutdown... killing input helper first.");
            stop_input_monitor();
            agent_log("Input monitor stopped, calling InitiateSystemShutdownExW...");
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox shutdown", 0, TRUE, FALSE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
            /* Keep connection open — host will see it drop when VM powers off */
        }
        else if (strcmp(cmd, "restart") == 0) {
            REPLY("ok");
            agent_log("Initiating restart...");
            stop_input_monitor();
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox restart", 0, TRUE, TRUE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
        }
        else if (strncmp(cmd, "gpu_query_response:", 19) == 0) {
            int share_count = atoi(cmd + 19);
            agent_log("Received GPU share list (%d shares).", share_count);
            handle_gpu_query_response(client, share_count);
        }
        else if (strcmp(cmd, "gpu_none") == 0) {
            agent_log("Host reports no GPU-PV assigned.");
        }
        else if (strcmp(cmd, "ssh_enable") == 0) {
            handle_ssh_enable(client, tag);
        }
        else if (strncmp(cmd, "set_ip:", 7) == 0) {
            /* set_ip:172.20.0.X/PREFIX:GATEWAY */
            char ip[32] = {0}, prefix[8] = {0}, gateway[32] = {0};
            char *slash, *colon2;
            char *arg = cmd + 7;

            /* Parse IP/prefix:gateway */
            slash = strchr(arg, '/');
            colon2 = slash ? strchr(slash, ':') : NULL;
            if (slash && colon2) {
                int ip_len = (int)(slash - arg);
                int pfx_len = (int)(colon2 - slash - 1);
                if (ip_len > 0 && ip_len < (int)sizeof(ip))
                    strncpy_s(ip, sizeof(ip), arg, ip_len);
                if (pfx_len > 0 && pfx_len < (int)sizeof(prefix))
                    strncpy_s(prefix, sizeof(prefix), slash + 1, pfx_len);
                strncpy_s(gateway, sizeof(gateway), colon2 + 1, sizeof(gateway) - 1);
            }

            if (ip[0] && prefix[0] && gateway[0]) {
                wchar_t wcmd[512];
                wchar_t nic[32] = L"Ethernet";
                ULONG nic_index = primary_nic_index();
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                DWORD exit_code = 1;

                if (nic_index)
                    swprintf_s(nic, 32, L"%lu", nic_index);

                swprintf_s(wcmd, 512,
                    L"netsh interface ip set address \"%s\" static %S %S %S",
                    nic, ip,
                    /* Convert prefix length to subnet mask */
                    atoi(prefix) == 16 ? "255.255.0.0" :
                    atoi(prefix) == 24 ? "255.255.255.0" :
                    atoi(prefix) == 8  ? "255.0.0.0" : "255.255.255.0",
                    gateway);

                agent_log("Setting IP: %S", wcmd);

                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));

                if (CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, 10000);
                    GetExitCodeProcess(pi.hProcess, &exit_code);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }

                /* Set DNS to gateway (host NAT) + 8.8.8.8 fallback */
                if (exit_code == 0) {
                    wchar_t dns_cmd[512];
                    STARTUPINFOW si2;
                    PROCESS_INFORMATION pi2;

                    swprintf_s(dns_cmd, 512,
                        L"netsh interface ip set dns \"%s\" static %S", nic, gateway);
                    ZeroMemory(&si2, sizeof(si2));
                    si2.cb = sizeof(si2);
                    ZeroMemory(&pi2, sizeof(pi2));
                    if (CreateProcessW(NULL, dns_cmd, NULL, NULL, FALSE,
                                       CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2)) {
                        WaitForSingleObject(pi2.hProcess, 10000);
                        CloseHandle(pi2.hProcess);
                        CloseHandle(pi2.hThread);
                    }

                    swprintf_s(dns_cmd, 512,
                        L"netsh interface ip add dns \"%s\" 8.8.8.8 index=2", nic);
                    ZeroMemory(&si2, sizeof(si2));
                    si2.cb = sizeof(si2);
                    ZeroMemory(&pi2, sizeof(pi2));
                    if (CreateProcessW(NULL, dns_cmd, NULL, NULL, FALSE,
                                       CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2)) {
                        WaitForSingleObject(pi2.hProcess, 10000);
                        CloseHandle(pi2.hProcess);
                        CloseHandle(pi2.hThread);
                    }

                    agent_log("IP configured: %s/%s gw %s dns %s,8.8.8.8",
                              ip, prefix, gateway, gateway);
                    REPLY("ok");
                } else {
                    agent_log("netsh failed (exit %lu)", exit_code);
                    REPLY("error:netsh_failed");
                }
            } else {
                agent_log("set_ip: bad format: %s", cmd);
                REPLY("error:bad_format");
            }
        }
        else if (strcmp(cmd, "set_dhcp") == 0) {
            wchar_t nic[32] = L"Ethernet";
            wchar_t wcmd[512];
            ULONG nic_index = primary_nic_index();
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            DWORD exit_code = 1;
            if (nic_index) swprintf_s(nic, ARRAYSIZE(nic), L"%lu", nic_index);

            swprintf_s(wcmd, ARRAYSIZE(wcmd),
                L"netsh interface ip set address \"%s\" dhcp", nic);
            ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                               NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 10000);
                GetExitCodeProcess(pi.hProcess, &exit_code);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
            if (exit_code == 0) {
                swprintf_s(wcmd, ARRAYSIZE(wcmd),
                    L"netsh interface ip set dns \"%s\" dhcp", nic);
                ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));
                exit_code = 1;
                if (CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                                   NULL, NULL, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, 10000);
                    GetExitCodeProcess(pi.hProcess, &exit_code);
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                }
            }
            if (exit_code == 0) {
                agent_log("Guest network set to DHCP on interface %S.", nic);
                REPLY("ok");
            } else {
                agent_log("Failed to set guest network to DHCP (exit %lu).", exit_code);
                REPLY("error:netsh_failed");
            }
        }
        else if (strcmp(cmd, "gpu_copy") == 0) {
            /* Host re-triggered GPU copy — ask for share list */
            REPLY("gpu_query");
        }
        else if (strcmp(cmd, "idd_connect") == 0) {
            handle_idd_connect(client, tag);
        }
        else {
            REPLY("error:unknown");
        }

        #undef REPLY
        }
    }

    /* A running GPU copy reports through send_to_host(), so after a plain
       disconnect it carries on and reports to the next connection. When the
       service is stopping, give it a moment to finish first. */
    if (g_gpu_copy.copying && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
        agent_log("Waiting for GPU copy thread to finish...");
        {
            int wait;
            for (wait = 0; wait < 10000 && g_gpu_copy.copying; wait += 500)
                Sleep(500);
        }
    }

    /* Unpublish under the send lock before closing: once this returns, no
       other thread is inside a send on this connection or can start one. */
    EnterCriticalSection(&g_send_cs);
    g_client_sock = NULL;
    LeaveCriticalSection(&g_send_cs);
    asb_close(client);
    agent_log("Client handler exiting.");
}

/* ---- Socket listener ---- */

static DWORD WINAPI listener_thread(LPVOID param)
{
    AsbListener *l;
    (void)param;

    if (asb_transport_init() != 0) {
        agent_log("asb_transport_init failed.");
        return 1;
    }

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        l = asb_listen(ASB_CH_AGENT);
        if (!l) {
            agent_log("asb_listen(ASB_CH_AGENT) failed, retrying in 3s");
            if (WaitForSingleObject(g_stop_event, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        agent_log("Listening on agent control channel (transport=%s)",
                  asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

        /* Accept loop — one client at a time */
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            AsbConn *client = asb_accept(l, 1000);   /* 1s timeout so we re-check g_stop_event */
            if (client) handle_client(client);
        }
        asb_close_listener(l);
    }

    agent_log("Listener stopped.");
    return 0;
}

/* ---- SSH: sshd detection + HV proxy (guest-side: HV socket → localhost:22) ---- */

/* SSH proxy service GUID: {A5B0CAFE-0007-4000-8000-000000000001} */
static const GUID SSH_SERVICE_GUID =
    { 0xa5b0cafe, 0x0007, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static HANDLE  g_ssh_proxy_thread = NULL;
static volatile BOOL g_ssh_proxy_running = FALSE;
static HANDLE  g_ssh_wait_thread = NULL;
static volatile BOOL g_ssh_waiting = FALSE;

#define SSH_RELAY_BUF 8192

typedef struct SshRelayCtx {
    AsbConn *hv;        /* transport connection to the host (AF_HYPERV on PC, ivshmem on Mac) */
    SOCKET   tcp_sock;  /* localhost:22 (sshd) */
} SshRelayCtx;

static DWORD WINAPI ssh_relay_thread(LPVOID param)
{
    SshRelayCtx *ctx = (SshRelayCtx *)param;
    char buf[SSH_RELAY_BUF];
    int n;
    SOCKET hv = (SOCKET)asb_conn_socket_u64(ctx->hv);

    if (hv != INVALID_SOCKET) {
        /* PC (AF_HYPERV): the connection exposes a real SOCKET, so relay both directions with a single dual-fd select(). */
        fd_set rfds;
        struct timeval tv;
        for (;;) {
            FD_ZERO(&rfds);
            FD_SET(hv, &rfds);
            FD_SET(ctx->tcp_sock, &rfds);
            tv.tv_sec  = 5;
            tv.tv_usec = 0;

            n = select(0, &rfds, NULL, NULL, &tv);
            if (n < 0) break;
            if (n == 0) continue;

            if (FD_ISSET(hv, &rfds)) {
                n = recv(hv, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (send(ctx->tcp_sock, buf, n, 0) != n) break;
            }
            if (FD_ISSET(ctx->tcp_sock, &rfds)) {
                n = recv(ctx->tcp_sock, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (send(hv, buf, n, 0) != n) break;
            }
        }
    } else {
        /* ivshmem: the connection has no fd to select() on, so relay each direction separately —
           poll the transport with a short timeout, non-blocking-check the TCP side. */
        fd_set rfds;
        struct timeval tv;
        for (;;) {
            int pr = asb_poll(ctx->hv, 5);
            if (pr < 0) break;
            if (pr > 0) {
                n = asb_recv(ctx->hv, buf, SSH_RELAY_BUF);
                if (n <= 0) break;
                { int off = 0; while (off < n) { int s = send(ctx->tcp_sock, buf + off, n - off, 0);
                    if (s <= 0) goto relay_done; off += s; } }
            }
            FD_ZERO(&rfds); FD_SET(ctx->tcp_sock, &rfds);
            tv.tv_sec = 0; tv.tv_usec = 0;
            if (select(0, &rfds, NULL, NULL, &tv) > 0) {
                n = recv(ctx->tcp_sock, buf, SSH_RELAY_BUF, 0);
                if (n <= 0) break;
                if (asb_send(ctx->hv, buf, n) != n) break;
            }
        }
    }
relay_done:
    asb_close(ctx->hv);
    closesocket(ctx->tcp_sock);
    free(ctx);
    return 0;
}

/* Check if sshd service is running. Returns TRUE if running. */
static BOOL is_sshd_running(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    BOOL running = FALSE;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;

    svc = OpenServiceA(scm, "sshd", SERVICE_QUERY_STATUS);
    if (svc) {
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
            running = TRUE;
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return running;
}

/* Try to start sshd and set it to auto-start. Returns TRUE if running. */
static BOOL ensure_sshd_running(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    BOOL ok = FALSE;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) return FALSE;

    svc = OpenServiceA(scm, "sshd", SERVICE_ALL_ACCESS);
    if (svc) {
        ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                             SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
            ok = TRUE;
        } else {
            if (StartServiceA(svc, 0, NULL))
                ok = TRUE;
            else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                ok = TRUE;
        }
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return ok;
}

/* HV proxy listener — runs after sshd is confirmed running */
static DWORD WINAPI ssh_proxy_thread(LPVOID param)
{
    AsbListener *l;
    struct sockaddr_in tcp_addr;
    (void)param;

    l = asb_listen(ASB_CH_SSH);
    if (!l) {
        agent_log("SSH proxy: asb_listen(ASB_CH_SSH) failed.");
        return 1;
    }
    agent_log("SSH proxy: listening on ssh channel (transport=%s)",
              asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    while (g_ssh_proxy_running) {
        AsbConn *client = asb_accept(l, 1000);   /* 1s so we re-check g_ssh_proxy_running */
        SOCKET tcp_s;
        if (!client) continue;

        /* Connect to local sshd */
        tcp_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_s == INVALID_SOCKET) { asb_close(client); continue; }

        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tcp_addr.sin_port = htons(22);

        if (connect(tcp_s, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) != 0) {
            agent_log("SSH proxy: connect to localhost:22 failed: %d", WSAGetLastError());
            closesocket(tcp_s);
            asb_close(client);
            continue;
        }

        /* Spawn relay thread */
        {
            SshRelayCtx *ctx = (SshRelayCtx *)malloc(sizeof(SshRelayCtx));
            if (ctx) {
                HANDLE t;
                ctx->hv       = client;
                ctx->tcp_sock = tcp_s;
                t = CreateThread(NULL, 0, ssh_relay_thread, ctx, 0, NULL);
                if (t) CloseHandle(t);  /* detached — relay cleans up itself */
                else { free(ctx); asb_close(client); closesocket(tcp_s); }
            } else {
                asb_close(client);
                closesocket(tcp_s);
            }
        }
    }

    asb_close_listener(l);
    agent_log("SSH proxy: stopped.");
    return 0;
}

static void start_ssh_proxy(void)
{
    if (g_ssh_proxy_thread) return;  /* already running */
    g_ssh_proxy_running = TRUE;
    g_ssh_proxy_thread = CreateThread(NULL, 0, ssh_proxy_thread, NULL, 0, NULL);
}

static void stop_ssh_proxy(void)
{
    g_ssh_proxy_running = FALSE;
    g_ssh_waiting = FALSE;
    if (g_ssh_wait_thread) {
        WaitForSingleObject(g_ssh_wait_thread, 5000);
        CloseHandle(g_ssh_wait_thread);
        g_ssh_wait_thread = NULL;
    }
    if (g_ssh_proxy_thread) {
        WaitForSingleObject(g_ssh_proxy_thread, 5000);
        CloseHandle(g_ssh_proxy_thread);
        g_ssh_proxy_thread = NULL;
    }
}

/* Background thread: poll for sshd service to become running.
   SetupComplete.cmd installs OpenSSH via MSI during first boot — sshd may
   not be running yet when the agent first handles ssh_enable. */
static DWORD WINAPI ssh_wait_thread(LPVOID param)
{
    int attempts = 0;
    (void)param;

    agent_log("SSH: waiting for sshd to become available...");

    while (g_ssh_waiting && attempts < 120) {  /* up to ~2 minutes */
        Sleep(1000);
        attempts++;

        if (ensure_sshd_running()) {
            agent_log("SSH: sshd is running (after %d seconds).", attempts);
            start_ssh_proxy();
            send_to_host("ssh_ready");
            g_ssh_waiting = FALSE;
            return 0;
        }
    }

    agent_log("SSH: sshd did not start within timeout.");
    send_to_host("ssh_failed");
    g_ssh_waiting = FALSE;
    return 1;
}

/* Handle "ssh_enable" command from host.
   sshd is installed by SetupComplete.cmd (MSI). Agent just detects + proxies. */
static void handle_ssh_enable(AsbConn *client, const char *tag)
{
    /* Already running? */
    if (is_sshd_running()) {
        agent_log("SSH: sshd already running.");
        send_reply(client, tag, "ssh_ready");
        start_ssh_proxy();
        return;
    }

    /* Try to start it (may be installed but stopped) */
    if (ensure_sshd_running()) {
        agent_log("SSH: sshd started successfully.");
        send_reply(client, tag, "ssh_ready");
        start_ssh_proxy();
        return;
    }

    /* Not installed yet — SetupComplete.cmd may still be running.
       Poll on a background thread. */
    if (g_ssh_waiting) {
        send_reply(client, tag, "ssh_installing");
        return;
    }

    g_ssh_waiting = TRUE;
    send_reply(client, tag, "ssh_installing");
    g_ssh_wait_thread = CreateThread(NULL, 0, ssh_wait_thread, NULL, 0, NULL);
    if (!g_ssh_wait_thread) {
        agent_log("SSH: failed to create wait thread.");
        g_ssh_waiting = FALSE;
        send_reply(client, tag, "ssh_failed");
    }
}

/* ---- Windows service plumbing ---- */

static void set_service_status(DWORD state, DWORD exit_code)
{
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI service_ctrl_ex(DWORD ctrl, DWORD event_type, LPVOID event_data, LPVOID context)
{
    (void)context;
    agent_log("service_ctrl received: %lu (event_type=%lu)", ctrl, event_type);

    /* The session is ending (or the service stopping) the orderly way:
       see check_previous_session(). */
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN ||
        ctrl == SERVICE_CONTROL_PRESHUTDOWN)
        mark_clean_shutdown(1);

    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        if (ctrl == SERVICE_CONTROL_SHUTDOWN)
            g_os_shutting_down = TRUE;

        /* Notify host */
        if (send_to_host(ctrl == SERVICE_CONTROL_SHUTDOWN ? "os_shutdown" : "service_stopping") > 0)
            agent_log("Sent notification to host (ctrl=%lu).", ctrl);

        set_service_status(SERVICE_STOP_PENDING, 0);
        agent_log("Setting stop event...");
        SetEvent(g_stop_event);
        return NO_ERROR;
    }

    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE) {
        WTSSESSION_NOTIFICATION *sn = (WTSSESSION_NOTIFICATION *)event_data;
        if (event_type == WTS_SESSION_LOGOFF) {
            agent_log("Session logoff detected (session %lu).",
                       sn ? sn->dwSessionId : 0);
            /* Do NOT restart the VDD device here.  IddCx handles the DWM
               transition naturally: UnassignSwapChain (user DWM dies) then
               AssignSwapChain (login-screen DWM starts compositing).
               A devcon restart mid-transition destroys the adapter/monitor
               and prevents IddCx from completing the handoff. */

            /* Disable Hyper-V Video after logoff (may have been re-enabled).
               devcon can be slow, so the device-check thread does it rather
               than this control handler. */
            request_device_check(&g_device_check_logoff);
        }
        if (event_type == WTS_SESSION_LOGON || event_type == WTS_SESSION_UNLOCK) {
            DWORD sid = sn ? sn->dwSessionId : WTSGetActiveConsoleSessionId();
            agent_log("Session %s detected (session %lu), respawning clipboard helpers.",
                       event_type == WTS_SESSION_LOGON ? "logon" : "unlock", sid);
            kill_clipboard_helper();
            if (sid != 0xFFFFFFFF)
                spawn_clipboard_in_session(sid);
            kill_clipboard_reader();
            if (sid != 0xFFFFFFFF)
                spawn_clipboard_reader_in_session(sid);
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPSTR *argv)
{
    HANDLE thread, device_thread;
    BOOL device_thread_done = TRUE;

    (void)argc; (void)argv;

    g_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl_ex, NULL);
    if (!g_status_handle) return;

    /* Init before accepting controls: the control handler uses the send lock
       (host notifications), the stop event and the device-check wake event;
       it and the clipboard monitor threads share g_clip_proc_cs for the
       helper-process globals. */
    InitializeCriticalSection(&g_clip_proc_cs);
    InitializeCriticalSection(&g_input_proc_cs);
    InitializeCriticalSection(&g_send_cs);
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_device_check_wake = CreateEventW(NULL, FALSE, FALSE, NULL);

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    set_service_status(SERVICE_START_PENDING, 0);

    /* p9copy and the GPU runtime setup log through agent_log */
    p9_set_log(agent_log);
    gl_vk_set_log(agent_log);

    /* Note how the previous Windows session ended (before any host connects) */
    check_previous_session();

    /* Start listener on a worker thread */
    thread = CreateThread(NULL, 0, listener_thread, NULL, 0, NULL);

    /* Start input helper monitor (spawns/respawns in console session as SYSTEM) */
    start_input_monitor();

    /* Start clipboard helper monitor (SYSTEM, :0005) */
    start_clipboard_monitor();

    /* Start clipboard reader monitor (user, :0006) */
    start_clipboard_reader_monitor();

    /* Start audio capture monitor (SYSTEM, :0004) */
    start_audio_monitor();

    set_service_status(SERVICE_RUNNING, 0);
    agent_log("Service started.");

    /* Device checks, starting with the service-start tasks (staged display
       driver update, ViGEmBus, VDD running, Hyper-V Video disabled). */
    device_thread = CreateThread(NULL, 0, device_check_thread, (LPVOID)1, 0, NULL);
    if (!device_thread)
        agent_log("Failed to create device-check thread (error %lu).", GetLastError());

    /* Wait until stop is signaled */
    WaitForSingleObject(g_stop_event, INFINITE);

    /* Clean up */
    agent_log("Stop event signaled, cleaning up...");
    agent_log("Stopping SSH proxy...");
    stop_ssh_proxy();
    agent_log("SSH proxy stopped.");
    agent_log("Stopping audio monitor...");
    stop_audio_monitor();
    agent_log("Audio monitor stopped.");
    agent_log("Stopping clipboard reader monitor...");
    stop_clipboard_reader_monitor();
    agent_log("Clipboard reader monitor stopped.");
    agent_log("Stopping clipboard monitor...");
    stop_clipboard_monitor();
    agent_log("Clipboard monitor stopped.");
    agent_log("Stopping input monitor...");
    stop_input_monitor();
    agent_log("Input monitor stopped.");
    /* The control listener is owned by listener_thread (asb_close_listener); its accept loop
       polls g_stop_event every 1s and exits on its own. Nothing socket-level to close here. */
    if (thread) {
        agent_log("Waiting for listener thread...");
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        agent_log("Listener thread exited.");
    }
    if (device_thread) {
        agent_log("Waiting for device-check thread...");
        device_thread_done = WaitForSingleObject(device_thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(device_thread);
        agent_log(device_thread_done ? "Device-check thread exited."
                                     : "Device-check thread still busy (devcon); leaving it to process exit.");
    }
    /* The locks and events stay alive: threads that are still finishing (GPU
       copy, a device check inside devcon, ViGEm setup, a connection thread
       waiting for the GPU copy) may still use them, and the process exits
       right after SERVICE_STOPPED. */

    agent_log("Service stopped.");
    set_service_status(SERVICE_STOPPED, 0);
}

/* ---- Install / Remove ---- */

static int install_service(void)
{
    SC_HANDLE scm, svc;
    char path[MAX_PATH];

    GetModuleFileNameA(NULL, path, MAX_PATH);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = CreateServiceA(scm, SERVICE_NAME, DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            printf("Service already exists.\n");
            svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_START);
        } else {
            printf("CreateService failed: %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return 1;
        }
    } else {
        printf("Service installed.\n");

        /* Set description */
        SERVICE_DESCRIPTIONA desc;
        desc.lpDescription = "AppSandbox guest agent for host-guest communication via Hyper-V sockets.";
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        /* Set recovery: restart on failure */
        SC_ACTION actions[3] = {
            { SC_ACTION_RESTART, 5000 },
            { SC_ACTION_RESTART, 10000 },
            { SC_ACTION_RESTART, 30000 }
        };
        SERVICE_FAILURE_ACTIONSA sfa = { 0 };
        sfa.dwResetPeriod = 86400;
        sfa.cActions = 3;
        sfa.lpsaActions = actions;
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
    }

    /* Start the service */
    if (svc) {
        if (StartServiceA(svc, 0, NULL))
            printf("Service started.\n");
        else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
            printf("Service already running.\n");
        else
            printf("StartService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return 0;
}

static int remove_service(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) {
        printf("Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    Sleep(1000);

    if (DeleteService(svc))
        printf("Service removed.\n");
    else
        printf("DeleteService failed: %lu\n", GetLastError());

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

/* ---- Entry point ---- */

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--install") == 0)
            return install_service();
        if (strcmp(argv[1], "--remove") == 0)
            return remove_service();
        printf("Usage: appsandbox-agent.exe [--install | --remove]\n");
        return 1;
    }

    /* Launched by SCM — run as service */
    {
        SERVICE_TABLE_ENTRYA table[] = {
            { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)service_main },
            { NULL, NULL }
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            /* Not running as service — run listener directly for debugging */
            HANDLE device_thread;
            printf("Running in console mode (Ctrl+C to stop)...\n");
            p9_set_log(agent_log);
            gl_vk_set_log(agent_log);
            InitializeCriticalSection(&g_send_cs);
            InitializeCriticalSection(&g_clip_proc_cs);
            InitializeCriticalSection(&g_input_proc_cs);
            g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            g_device_check_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
            /* Connection-time and periodic device checks only (no service-start tasks) */
            device_thread = CreateThread(NULL, 0, device_check_thread, NULL, 0, NULL);
            listener_thread(NULL);
            SetEvent(g_stop_event);
            if (device_thread) {
                WaitForSingleObject(device_thread, INFINITE);
                CloseHandle(device_thread);
            }
            /* Locks and events stay alive until the process exits (see
               service_main). */
        }
    }
    return 0;
}
