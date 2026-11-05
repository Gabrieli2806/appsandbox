/*
 * guest_tools.c -- Refresh the AppSandbox guest components inside an
 * existing Windows VM's disk.
 *
 * Guest components (agent, session helpers, VDD display driver) are copied
 * into a VM's disk only when the VM is created, so VMs made with an older
 * App Sandbox keep older components. This module updates them offline, with
 * the VM stopped:
 *
 *   1. attach the VM's current VHDX on the host (no drive letter);
 *   2. find the Windows volume on it (the one holding \Windows\AppSandbox);
 *   3. refuse when that volume holds a hibernation image (Fast Startup or
 *      hibernate): Windows would resume a stale copy of the file system
 *      over our changes and could corrupt it;
 *   4. copy the guest executables over \Windows\AppSandbox\*.exe and the VDD
 *      package into \Windows\AppSandbox\drivers, then drop vdd-update.pending;
 *   5. detach.
 *
 * On the next boot the (new) agent sees vdd-update.pending, trusts the
 * driver's certificate and runs `devcon update` for Root\AppSandboxVDD.
 *
 * A signature of the host's guest files is stored beside the VM's disk
 * (guest_tools.sig) so the automatic check before each start only attaches
 * the disk when the host has newer components than the VM last received.
 */

#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <stdio.h>
#include <wchar.h>

#include "guest_tools.h"
#include "asb_core.h"

#pragma comment(lib, "virtdisk.lib")

/* Guest executables shipped under resources\ (same set the disk builder stages). */
static const wchar_t *g_guest_bins[] = {
    L"appsandbox-agent.exe", L"appsandbox-input.exe", L"appsandbox-displays.exe",
    L"appsandbox-clipboard.exe", L"appsandbox-clipboard-reader.exe", L"appsandbox-audio.exe"
};
/* VDD package under drivers\. The .cer is required: each build signs the
   driver with its own test certificate. */
static const wchar_t *g_vdd_files[] = {
    L"AppSandboxVDD.dll", L"AppSandboxVDD.inf", L"AppSandboxVDD.cat",
    L"AppSandboxVDD.cer", L"devcon.exe"
};

#define VDD_PENDING_MARKER L"vdd-update.pending"

/* ---- Host-side sources ---- */

static void exe_dir(wchar_t *out, size_t cap)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, out, (DWORD)cap);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

/* resources\ and drivers\ beside the app. FALSE when either is missing. */
static BOOL host_dirs(wchar_t *res, wchar_t *drv, size_t cap)
{
    wchar_t exe[MAX_PATH];
    exe_dir(exe, MAX_PATH);
    swprintf_s(res, cap, L"%s\\resources", exe);
    swprintf_s(drv, cap, L"%s\\drivers", exe);
    return GetFileAttributesW(res) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(drv) != INVALID_FILE_ATTRIBUTES;
}

static void fnv1a(UINT64 *h, const BYTE *p, DWORD n)
{
    DWORD i;
    for (i = 0; i < n; i++) { *h ^= p[i]; *h *= 1099511628211ULL; }
}

/* FNV-1a over the contents of every file the update copies. The files do
   not change while the app runs, so the result is computed once. */
static BOOL host_signature_compute(UINT64 *sig);

static BOOL host_signature(UINT64 *sig)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static UINT64 cached;
    static BOOL cached_ok;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        cached_ok = host_signature_compute(&cached);
        InitOnceComplete(&once, 0, NULL);
    }
    if (cached_ok) *sig = cached;
    return cached_ok;
}

static BOOL host_signature_compute(UINT64 *sig)
{
    wchar_t res[MAX_PATH], drv[MAX_PATH], path[MAX_PATH];
    BYTE buf[65536];
    UINT64 h = 1469598103934665603ULL;
    int i;

    if (!host_dirs(res, drv, MAX_PATH)) return FALSE;
    for (i = 0; i < (int)(ARRAYSIZE(g_guest_bins) + ARRAYSIZE(g_vdd_files)); i++) {
        HANDLE f;
        DWORD got;
        if (i < (int)ARRAYSIZE(g_guest_bins))
            swprintf_s(path, MAX_PATH, L"%s\\%s", res, g_guest_bins[i]);
        else
            swprintf_s(path, MAX_PATH, L"%s\\%s", drv, g_vdd_files[i - ARRAYSIZE(g_guest_bins)]);
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return FALSE;
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got > 0) fnv1a(&h, buf, got);
        CloseHandle(f);
    }
    *sig = h;
    return TRUE;
}

static void sig_path(const VmInstance *vm, wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH], *slash;
    wcscpy_s(dir, MAX_PATH, vm->vhdx_path);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0';
    swprintf_s(out, cap, L"%s\\guest_tools.sig", dir);
}

static BOOL read_vm_signature(const VmInstance *vm, UINT64 *sig)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    unsigned long long v = 0;
    BOOL ok;
    sig_path(vm, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return FALSE;
    ok = fscanf_s(f, "%llx", &v) == 1;
    fclose(f);
    if (ok) *sig = v;
    return ok;
}

static void write_vm_signature(const VmInstance *vm, UINT64 sig)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    sig_path(vm, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "%016llx\n", (unsigned long long)sig);
    fclose(f);
}

BOOL guest_tools_applies(const VmInstance *vm)
{
    return vm && _wcsicmp(vm->os_type, L"Windows") == 0 && !vm->is_template;
}

BOOL guest_tools_outdated(const VmInstance *vm)
{
    UINT64 host = 0, have = 0;
    if (!guest_tools_applies(vm) || !vm->install_complete) return FALSE;
    if (!host_signature(&host)) return FALSE;
    return !read_vm_signature(vm, &have) || have != host;
}

/* ---- Offline volume access ---- */

static BOOL disk_number_from_path(const wchar_t *physical, DWORD *number)
{
    const wchar_t *p = physical + wcslen(physical);
    while (p > physical && iswdigit(p[-1])) p--;
    if (!*p) return FALSE;
    *number = (DWORD)_wtoi(p);
    return TRUE;
}

/* The volume on disk `number` that holds \Windows\AppSandbox. vol receives
   its GUID path with a trailing backslash. Volumes appear a moment after the
   attach, so this polls for a few seconds. */
static BOOL find_windows_volume(DWORD number, wchar_t *vol, size_t cap)
{
    int attempt;
    for (attempt = 0; attempt < 40; attempt++) {
        wchar_t name[MAX_PATH];
        HANDLE find = FindFirstVolumeW(name, MAX_PATH);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                size_t len = wcslen(name);
                HANDLE h;
                BYTE ext_buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
                VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)ext_buf;
                DWORD got = 0, i;
                BOOL on_disk = FALSE;

                if (len < 2) continue;
                name[len - 1] = L'\0';           /* CreateFile wants no trailing slash */
                h = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                name[len - 1] = L'\\';
                if (h == INVALID_HANDLE_VALUE) continue;
                if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                                    ext, sizeof(ext_buf), &got, NULL)) {
                    for (i = 0; i < ext->NumberOfDiskExtents; i++)
                        if (ext->Extents[i].DiskNumber == number) on_disk = TRUE;
                }
                CloseHandle(h);
                if (on_disk) {
                    wchar_t probe[MAX_PATH];
                    swprintf_s(probe, MAX_PATH, L"%sWindows\\AppSandbox", name);
                    if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
                        wcscpy_s(vol, cap, name);
                        FindVolumeClose(find);
                        return TRUE;
                    }
                }
            } while (FindNextVolumeW(find, name, MAX_PATH));
            FindVolumeClose(find);
        }
        Sleep(250);
    }
    return FALSE;
}

/* TRUE when hiberfil.sys holds a resumable image ("HIBR"/"hibr"). Fast
   Startup shutdowns and hibernation leave one; a full shutdown or restart
   does not. */
static BOOL volume_hibernated(const wchar_t *vol)
{
    wchar_t path[MAX_PATH];
    char sig[4] = { 0 };
    DWORD got = 0;
    HANDLE f;
    swprintf_s(path, MAX_PATH, L"%shiberfil.sys", vol);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(f, sig, sizeof(sig), &got, NULL);
    CloseHandle(f);
    return got == sizeof(sig) && _strnicmp(sig, "HIBR", 4) == 0;
}

/* The VM's disk is mounted on the host with this process's administrator
   rights, and its contents are controlled by the guest. A junction or
   symbolic link planted in the guest (e.g. Windows\AppSandbox -> C:\Windows)
   would resolve to the HOST's paths. Refuse every reparse point on the way
   to what we write. Missing components are fine (they are created). */
static BOOL guest_path_safe(const wchar_t *vol, const wchar_t *rel)
{
    wchar_t path[MAX_PATH], part[MAX_PATH], *ctx = NULL, *tok;
    DWORD a;
    wcscpy_s(path, MAX_PATH, vol);                /* "\\?\Volume{...}\" */
    wcscpy_s(part, MAX_PATH, rel);
    for (tok = wcstok_s(part, L"\\", &ctx); tok; tok = wcstok_s(NULL, L"\\", &ctx)) {
        size_t n = wcslen(path);
        if (n && path[n - 1] != L'\\') wcscat_s(path, MAX_PATH, L"\\");
        wcscat_s(path, MAX_PATH, tok);
        a = GetFileAttributesW(path);
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    }
    return TRUE;
}

static BOOL copy_into(const wchar_t *src_dir, const wchar_t *name, const wchar_t *dst_dir,
                      wchar_t *err, size_t err_cap)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    DWORD a;
    swprintf_s(src, MAX_PATH, L"%s\\%s", src_dir, name);
    swprintf_s(dst, MAX_PATH, L"%s\\%s", dst_dir, name);
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) return TRUE;  /* optional helper */
    a = GetFileAttributesW(dst);
    if (a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        swprintf_s(err, err_cap, L"%s in the VM is a link or folder; the update was refused for safety.", name);
        return FALSE;
    }
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src, dst, FALSE)) {
        swprintf_s(err, err_cap, L"Could not copy %s into the VM (error %lu).", name, GetLastError());
        return FALSE;
    }
    return TRUE;
}

HRESULT guest_tools_update(VmInstance *vm, wchar_t *err, size_t err_cap)
{
    VIRTUAL_STORAGE_TYPE vst = { VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN, { 0 } };
    OPEN_VIRTUAL_DISK_PARAMETERS op;
    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    HANDLE disk = INVALID_HANDLE_VALUE;
    wchar_t res[MAX_PATH], drv[MAX_PATH], physical[MAX_PATH], vol[MAX_PATH];
    wchar_t app_dir[MAX_PATH], drv_dir[MAX_PATH], marker[MAX_PATH];
    ULONG physical_size = sizeof(physical);
    DWORD number = 0, rc;
    BOOL attached = FALSE;
    HRESULT hr = E_FAIL;
    UINT64 sig = 0;
    int i;

    err[0] = L'\0';
    if (!guest_tools_applies(vm)) {
        wcscpy_s(err, err_cap, L"Guest tools updates are for Windows VMs.");
        return E_INVALIDARG;
    }
    if (!host_dirs(res, drv, MAX_PATH) || !host_signature(&sig)) {
        wcscpy_s(err, err_cap, L"This App Sandbox folder is missing its resources or drivers folder.");
        return E_FAIL;
    }

    ZeroMemory(&op, sizeof(op));
    op.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    rc = OpenVirtualDisk(&vst, vm->vhdx_path, VIRTUAL_DISK_ACCESS_NONE,
                         OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &disk);
    if (rc != ERROR_SUCCESS) {
        swprintf_s(err, err_cap, L"Could not open the VM's disk (error %lu). Is the VM fully stopped?", rc);
        return HRESULT_FROM_WIN32(rc);
    }

    ZeroMemory(&ap, sizeof(ap));
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    rc = AttachVirtualDisk(disk, NULL, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &ap, NULL);
    if (rc != ERROR_SUCCESS) {
        swprintf_s(err, err_cap, L"Could not attach the VM's disk (error %lu).", rc);
        hr = HRESULT_FROM_WIN32(rc);
        goto done;
    }
    attached = TRUE;

    rc = GetVirtualDiskPhysicalPath(disk, &physical_size, physical);
    if (rc != ERROR_SUCCESS || !disk_number_from_path(physical, &number)) {
        swprintf_s(err, err_cap, L"Could not find the attached disk (error %lu).", rc);
        goto done;
    }
    if (!find_windows_volume(number, vol, MAX_PATH)) {
        wcscpy_s(err, err_cap, L"No App Sandbox Windows installation was found on the VM's disk "
                               L"(it may be encrypted with BitLocker).");
        goto done;
    }
    if (volume_hibernated(vol)) {
        wcscpy_s(err, err_cap, L"Windows in this VM was hibernated or shut down with Fast Startup, so its disk "
                               L"cannot be changed safely. Start the VM, choose Restart inside Windows (or "
                               L"Shut down from App Sandbox), then try again.");
        hr = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        goto done;
    }

    if (!guest_path_safe(vol, L"Windows\\AppSandbox\\drivers\\" VDD_PENDING_MARKER)) {
        wcscpy_s(err, err_cap, L"The VM's App Sandbox folder contains a link (junction or symbolic link); "
                               L"the update was refused for safety. Remove the link inside the VM.");
        hr = E_ACCESSDENIED;
        goto done;
    }
    swprintf_s(app_dir, MAX_PATH, L"%sWindows\\AppSandbox", vol);
    swprintf_s(drv_dir, MAX_PATH, L"%s\\drivers", app_dir);
    CreateDirectoryW(drv_dir, NULL);
    if (!guest_path_safe(vol, L"Windows\\AppSandbox\\drivers")) {   /* re-check after creating */
        wcscpy_s(err, err_cap, L"The VM's App Sandbox folder contains a link; the update was refused.");
        hr = E_ACCESSDENIED;
        goto done;
    }

    for (i = 0; i < (int)ARRAYSIZE(g_guest_bins); i++)
        if (!copy_into(res, g_guest_bins[i], app_dir, err, err_cap)) goto done;
    for (i = 0; i < (int)ARRAYSIZE(g_vdd_files); i++)
        if (!copy_into(drv, g_vdd_files[i], drv_dir, err, err_cap)) goto done;
    /* Optional: a ViGEmBus installer placed in drivers\ (game controllers);
       the agent installs it when the bus is missing. */
    if (!copy_into(drv, L"ViGEmBus_setup.exe", drv_dir, err, err_cap)) goto done;

    swprintf_s(marker, MAX_PATH, L"%s\\%s", drv_dir, VDD_PENDING_MARKER);
    {
        /* FILE_FLAG_OPEN_REPARSE_POINT: never follow a link planted as the marker. */
        HANDLE m = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (m == INVALID_HANDLE_VALUE) {
            swprintf_s(err, err_cap, L"Could not schedule the display driver update (error %lu).", GetLastError());
            goto done;
        }
        CloseHandle(m);
    }

    hr = S_OK;

done:
    if (attached) DetachVirtualDisk(disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    if (disk != INVALID_HANDLE_VALUE) CloseHandle(disk);
    if (SUCCEEDED(hr)) write_vm_signature(vm, sig);
    return hr;
}
