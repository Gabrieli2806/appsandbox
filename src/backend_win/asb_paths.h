/*
 * asb_paths.h -- where App Sandbox may write, and the machine-wide objects
 * every App Sandbox process shares (data folder, log, single-instance lock).
 *
 * A zip install keeps caches beside AppSandbox.exe. An MSIX install lives in
 * the read-only WindowsApps folder, so caches, logs and files shared into
 * VMs go to %ProgramData%\AppSandbox instead.
 */
#ifndef ASB_PATHS_H
#define ASB_PATHS_H

#include <windows.h>
#include <wchar.h>
#include <sddl.h>
#include <aclapi.h>

typedef LONG (WINAPI *PFN_AsbGetCurrentPackageFullName)(UINT32 *, PWSTR);

/* TRUE when this process runs from an MSIX package. */
static __inline BOOL asb_running_packaged(void)
{
    static LONG cached = -1;
    if (cached < 0) {
        PFN_AsbGetCurrentPackageFullName fn = (PFN_AsbGetCurrentPackageFullName)(void *)
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName");
        UINT32 len = 0;
        LONG rc = fn ? fn(&len, NULL) : APPMODEL_ERROR_NO_PACKAGE;
        InterlockedExchange(&cached, rc == ERROR_INSUFFICIENT_BUFFER ? 1 : 0);
    }
    return cached == 1;
}

/* Access to %ProgramData%\AppSandbox: SYSTEM and Administrators full
   control, Users read and execute. Protected, so ProgramData's default
   (every user may create files and folders) is not inherited. The log stays
   readable without elevation; host.json, ssh\ and the VM folders carry their
   own owner-only DACLs. */
#define ASB_DATA_DIR_SDDL L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"

/* %ProgramData%\AppSandbox, without creating it. */
static __inline void asb_program_data_path(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (!n || n >= MAX_PATH)
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, cch, L"%s\\AppSandbox", base);
}

/* Create a folder with ASB_DATA_DIR_SDDL as its DACL. */
static __inline BOOL asb_create_data_dir(const wchar_t *path)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    PSECURITY_DESCRIPTOR sd = NULL;
    BOOL ok;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(ASB_DATA_DIR_SDDL,
                                                             SDDL_REVISION_1, &sd, NULL))
        sa.lpSecurityDescriptor = sd;
    ok = CreateDirectoryW(path, &sa);
    if (sd) LocalFree(sd);
    return ok;
}

/* %ProgramData%\AppSandbox (created with ASB_DATA_DIR_SDDL when missing;
   main.c repairs an existing one at startup). */
static __inline void asb_program_data_dir(wchar_t *out, size_t cch)
{
    asb_program_data_path(out, cch);
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES)
        asb_create_data_dir(out);
}

/* %ProgramData%\AppSandbox\appsandbox.log, by absolute path in every mode
   (the working directory is not the app or data folder). */
static __inline void asb_log_path(wchar_t *out, size_t cch)
{
    asb_program_data_path(out, cch);
    wcscat_s(out, cch, L"\\appsandbox.log");
}

/* Directory for caches App Sandbox writes: beside the exe for a zip
   install, %ProgramData%\AppSandbox for an MSIX install. */
static __inline void asb_writable_dir(wchar_t *out, size_t cch)
{
    wchar_t *slash;
    if (asb_running_packaged()) {
        asb_program_data_dir(out, cch);
        return;
    }
    GetModuleFileNameW(NULL, out, (DWORD)cch);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

/* TRUE when a SID is SYSTEM, Administrators or the user running this
   process: the owners objects created by App Sandbox can have. */
static __inline BOOL asb_owner_sid_trusted(PSID owner)
{
    union { TOKEN_USER user; BYTE raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } token_user;
    HANDLE token;
    DWORD len = 0;
    BOOL trusted = FALSE;
    if (!owner || !IsValidSid(owner)) return FALSE;
    if (IsWellKnownSid(owner, WinLocalSystemSid) ||
        IsWellKnownSid(owner, WinBuiltinAdministratorsSid))
        return TRUE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenUser, &token_user, sizeof(token_user), &len))
            trusted = EqualSid(owner, token_user.user.User.Sid);
        CloseHandle(token);
    }
    return trusted;
}

/* asb_instance_lock results. */
#define ASB_INSTANCE_FREE     0   /* this process now holds the lock */
#define ASB_INSTANCE_RUNNING  1   /* another App Sandbox process holds it */

/* Single-instance lock shared by the app window and the headless daemon
   (both use the one vms.cfg and the HCN networks asb_init clears).

   Every version takes the mutex Global\AppSandboxCoreHost, so an older
   version that runs is still seen. Any local user can create a Global\
   object first, though, so a mutex found there counts only when SYSTEM,
   Administrators or this user own it; one squatted by anyone else is
   ignored instead of blocking App Sandbox. A second mutex lives in a private
   namespace bounded to the Administrators group, which standard users can
   neither create nor open, and keeps two current instances apart even when
   the global name is squatted. The handles stay open for the life of the
   process (the system releases them when it exits). */
static __inline int asb_instance_lock(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    PSECURITY_DESCRIPTOR sd = NULL;
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD admins_len = sizeof(admins);
    HANDLE boundary, ns, mutex;
    int result = ASB_INSTANCE_FREE;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
                                                             SDDL_REVISION_1, &sd, NULL))
        sa.lpSecurityDescriptor = sd;

    boundary = CreateBoundaryDescriptorW(L"AppSandbox", 0);
    if (boundary &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins, &admins_len) &&
        AddSIDToBoundaryDescriptor(&boundary, admins)) {
        ns = CreatePrivateNamespaceW(&sa, boundary, L"AppSandbox");
        if (!ns && GetLastError() == ERROR_ALREADY_EXISTS)
            ns = OpenPrivateNamespaceW(boundary, L"AppSandbox");
        if (ns) {
            mutex = CreateMutexW(&sa, FALSE, L"AppSandbox\\CoreHost");
            if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
                result = ASB_INSTANCE_RUNNING;
        }
    }

    mutex = CreateMutexW(&sa, FALSE, L"Global\\AppSandboxCoreHost");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        PSID owner = NULL;
        PSECURITY_DESCRIPTOR owner_sd = NULL;
        BOOL trusted = GetSecurityInfo(mutex, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                                       &owner, NULL, NULL, NULL, &owner_sd) == ERROR_SUCCESS &&
                       asb_owner_sid_trusted(owner);
        if (owner_sd) LocalFree(owner_sd);
        if (trusted) result = ASB_INSTANCE_RUNNING;
        else CloseHandle(mutex);
    }

    if (sd) LocalFree(sd);
    return result;
}

#endif
