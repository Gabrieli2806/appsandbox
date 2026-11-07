#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <stdio.h>
#include "ui.h"
#include "headless.h"
#include "asb_paths.h"

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

/* Command-line options, parsed with the shell's own rules (quotes, escaped
   quotes), so a VM name is never mistaken for an option:
     --headless             run the headless daemon (also -headless)
     --vm <name>            start or show that VM (desktop/Start menu shortcuts)
   The value after --vm is taken as the name even when it looks like an
   option, e.g. a VM called "--headless". */
static void parse_command_line(BOOL *headless, wchar_t *vm_name, size_t vm_name_cap)
{
    int argc = 0, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    *headless = FALSE;
    vm_name[0] = L'\0';
    if (!argv) return;
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--vm") == 0) {
            if (i + 1 < argc)
                wcsncpy_s(vm_name, vm_name_cap, argv[++i], _TRUNCATE);
        } else if (_wcsicmp(argv[i], L"--headless") == 0 ||
                   _wcsicmp(argv[i], L"-headless") == 0) {
            *headless = TRUE;
        }
    }
    LocalFree(argv);
}

/* Hand a VM name to the running instance. */
static BOOL send_vm_to_running_instance(const wchar_t *name)
{
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        HWND existing = FindWindowW(UI_MAIN_WINDOW_CLASS, NULL);
        if (existing) {
            COPYDATASTRUCT cds;
            DWORD pid = 0;
            DWORD_PTR result = 0;
            GetWindowThreadProcessId(existing, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            cds.dwData = UI_COPYDATA_LAUNCH_VM;
            cds.cbData = (DWORD)((wcslen(name) + 1) * sizeof(wchar_t));
            cds.lpData = (PVOID)name;
            if (SendMessageTimeoutW(existing, WM_COPYDATA, 0, (LPARAM)&cds,
                                    SMTO_ABORTIFHUNG, 5000, &result))
                return TRUE;
        }
        Sleep(250);
    }
    return FALSE;
}

/* ---- %ProgramData%\AppSandbox access ---- */

static BOOL process_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    BOOL elevated = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

/* Enable or disable the privileges an administrator needs to take over
   objects another user owns, whatever their DACL says. */
static void set_takeover_privileges(BOOL enable)
{
    static const wchar_t *names[] = { SE_TAKE_OWNERSHIP_NAME, SE_RESTORE_NAME, SE_BACKUP_NAME };
    HANDLE token;
    int i;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    for (i = 0; i < (int)ARRAYSIZE(names); i++) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
        if (LookupPrivilegeValueW(NULL, names[i], &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(token);
}

/* Open a file or folder itself (never what a reparse point leads to) for
   security changes. */
static HANDLE open_for_security(const wchar_t *path, DWORD access)
{
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

/* NT VIRTUAL MACHINE\<id> (S-1-5-83-...): the account of a Hyper-V worker
   process, which may create files in a VM folder. Not an interactive user. */
static BOOL sid_is_vm_worker(PSID sid)
{
    static const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID_IDENTIFIER_AUTHORITY authority;
    if (!sid || !IsValidSid(sid) || *GetSidSubAuthorityCount(sid) < 1) return FALSE;
    authority = GetSidIdentifierAuthority(sid);
    return memcmp(authority, &nt_authority, sizeof(nt_authority)) == 0 &&
           *GetSidSubAuthority(sid, 0) == 83;   /* SECURITY_VIRTUALSERVER_ID_BASE_RID */
}

/* Owner check of the object itself (a link, not its target); FALSE when
   the owner cannot be read. */
static BOOL object_owner_trusted(const wchar_t *path)
{
    HANDLE h = open_for_security(path, READ_CONTROL);
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner = NULL;
    BOOL trusted = FALSE;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, NULL, NULL, NULL, &sd) == ERROR_SUCCESS)
        trusted = asb_owner_sid_trusted(owner) || sid_is_vm_worker(owner);
    if (sd) LocalFree(sd);
    CloseHandle(h);
    return trusted;
}

/* Make Administrators the owner of an object someone else created here and
   give it a protected SYSTEM + Administrators DACL, so its creator can no
   longer change it. */
static void take_over_object(const wchar_t *path, BOOL directory)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD admins_len = sizeof(admins);
    HANDLE h;
    DWORD rc = ERROR_INVALID_PARAMETER;

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins, &admins_len) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            directory ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" : L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",
            SDDL_REVISION_1, &sd, NULL))
        return;
    h = open_for_security(path, READ_CONTROL | WRITE_DAC | WRITE_OWNER);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present && dacl)
            rc = SetSecurityInfo(h, SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                 PROTECTED_DACL_SECURITY_INFORMATION,
                                 (PSID)admins, NULL, dacl, NULL);
        CloseHandle(h);
    } else {
        rc = GetLastError();
    }
    LocalFree(sd);
    if (rc == ERROR_SUCCESS)
        ui_log(L"App data: %s was created by another user; it now belongs to Administrators.", path);
    else
        ui_log(L"App data: cannot take over %s, which another user created (error %lu).", path, rc);
}

/* Walk the data folder (never through links) and neutralize what another
   user created while the folder still let every user write: their links are
   removed (the link only, never its target) and their files and folders are
   taken over. Objects App Sandbox created are left as they are. */
static void take_over_untrusted(const wchar_t *dir, int depth)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    /* _TRUNCATE: a path too long for MAX_PATH is skipped, never fatal. */
    if (depth > 16 || _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir) < 0) return;
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        BOOL is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        BOOL is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0) continue;
        if (!object_owner_trusted(path)) {
            if (is_link) {
                BOOL removed = is_dir ? RemoveDirectoryW(path) : DeleteFileW(path);
                ui_log(removed ? L"App data: removed link %s, which another user created."
                               : L"App data: cannot remove link %s, which another user created.",
                       path);
                continue;
            }
            take_over_object(path, is_dir);
        }
        if (is_dir && !is_link) take_over_untrusted(path, depth + 1);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

/* Owner and DACL state of the folder a path leads to (an administrator's
   link to another drive is followed). */
static BOOL folder_secured(const wchar_t *path)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    BOOL secured = FALSE;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, NULL, &dacl, NULL, &sd) == ERROR_SUCCESS) {
        secured = asb_owner_sid_trusted(owner) && dacl &&
                  GetSecurityDescriptorControl(sd, &control, &revision) &&
                  (control & SE_DACL_PROTECTED) != 0;
    }
    if (sd) LocalFree(sd);
    return secured;
}

/* Create %ProgramData%\AppSandbox with its own DACL, or repair one created
   before App Sandbox protected it (it inherited ProgramData's ACL, which
   lets every local user add files and folders the elevated app would then
   trust). Runs at every start; a folder that is already protected and owned
   by SYSTEM, Administrators or this user is left alone, so the repair and
   its walk happen once. */
static void secure_data_dir(void)
{
    wchar_t dir[MAX_PATH], aside[MAX_PATH];
    DWORD attrs;
    BOOL link_or_file;
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD admins_len = sizeof(admins), rc;

    if (!process_elevated()) return;
    asb_program_data_path(dir, MAX_PATH);
    attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        asb_create_data_dir(dir);
        return;
    }

    link_or_file = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (link_or_file && !object_owner_trusted(dir)) {
        /* A link or file another user put in place of the folder: move it
           aside (a link is renamed, its target is not touched) and start a
           new folder. */
        swprintf_s(aside, MAX_PATH, L"%s.untrusted-%08lX", dir, GetTickCount());
        if (MoveFileExW(dir, aside, 0)) {
            asb_create_data_dir(dir);
            ui_log(L"App data: %s was not created by an administrator; moved to %s.", dir, aside);
        } else {
            ui_log(L"App data: %s was not created by an administrator and cannot be moved (error %lu).",
                   dir, GetLastError());
        }
        return;
    }
    if (folder_secured(dir)) return;

    /* SetNamedSecurityInfo also propagates the new inherited entries to
       everything below that inherits (VM folders, host.json and ssh\ keep
       their own protected DACLs). */
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins, &admins_len) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(ASB_DATA_DIR_SDDL, SDDL_REVISION_1, &sd, NULL))
        return;
    set_takeover_privileges(TRUE);
    rc = ERROR_INVALID_PARAMETER;
    if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present && dacl) {
        rc = SetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                   PROTECTED_DACL_SECURITY_INFORMATION,
                                   (PSID)admins, NULL, dacl, NULL);
        if (rc != ERROR_SUCCESS && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            /* A folder another user created with a DACL that shuts
               administrators out: take ownership through backup semantics
               first. */
            take_over_object(dir, TRUE);
            rc = SetNamedSecurityInfoW(dir, SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                       PROTECTED_DACL_SECURITY_INFORMATION,
                                       (PSID)admins, NULL, dacl, NULL);
        }
    }
    LocalFree(sd);
    if (rc == ERROR_SUCCESS) {
        ui_log(L"App data: only administrators can now write to %s.", dir);
        take_over_untrusted(dir, 0);
    } else {
        ui_log(L"App data: cannot set the access list of %s (error %lu).", dir, rc);
    }
    set_takeover_privileges(FALSE);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    HWND hwnd;
    MSG msg;
    wchar_t launch_vm[256] = L"";
    wchar_t system_dir[MAX_PATH];
    BOOL headless = FALSE;

    (void)hPrevInstance;

    /* Never load DLLs from the current directory; system DLLs are loaded
       from System32 explicitly (LOAD_LIBRARY_SEARCH_SYSTEM32). */
    SetDllDirectoryW(L"");

    /* Programs started by name (cmd.exe, ssh) are never looked up in the
       current directory, and that directory is System32, never a folder
       other users can write to. Logs and data use absolute paths. */
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", L"1");
    if (GetSystemDirectoryW(system_dir, MAX_PATH))
        SetCurrentDirectoryW(system_dir);

    /* Per-monitor DPI awareness (Windows 10 1703+) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Initialize OLE/COM (required for WebView2) */
    OleInitialize(NULL);

    secure_data_dir();

    parse_command_line(&headless, launch_vm, ARRAYSIZE(launch_vm));

    /* Headless daemon mode: appsandbox.exe --headless
       (normal launches pass no args, so the GUI path below is unaffected). */
    if (headless) {
        int rc = run_headless(hInstance, pCmdLine);
        OleUninitialize();
        return rc;
    }

    /* Single-instance: one AppSandbox (GUI or headless) at a time. The headless
       daemon (headless.c) takes the SAME lock, so GUI + headless are mutually
       exclusive -- required because both touch the one %ProgramData%\AppSandbox\
       vms.cfg (no file locking) and the machine-global HCN networks that asb_init
       wipes (hcn_cleanup_stale_networks). The lock is machine-wide (any folder,
       any session) and the OS releases it on crash. */
    if (asb_instance_lock() == ASB_INSTANCE_RUNNING) {
        /* The running instance may be the app hidden in the tray (closing
           the window while VMs run keeps it alive). Ask it to show its
           window instead of refusing to open. */
        if (launch_vm[0] ? send_vm_to_running_instance(launch_vm) : show_running_instance()) {
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

    /* Create main window */
    /* From a VM shortcut the VM window is what matters: keep the manager
       window minimized. */
    hwnd = ui_create_main_window(hInstance, launch_vm[0] ? SW_SHOWMINNOACTIVE : nCmdShow);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", L"App Sandbox", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }
    if (launch_vm[0])
        ui_launch_vm_by_name(launch_vm);

    /* Message loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return (int)msg.wParam;
}
