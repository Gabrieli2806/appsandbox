/*
 * asb_paths.h -- where App Sandbox may write.
 *
 * A zip install keeps caches beside AppSandbox.exe. An MSIX install lives in
 * the read-only WindowsApps folder, so caches, logs and files shared into
 * VMs go to %ProgramData%\AppSandbox instead.
 */
#ifndef ASB_PATHS_H
#define ASB_PATHS_H

#include <windows.h>
#include <wchar.h>

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

/* %ProgramData%\AppSandbox (created). */
static __inline void asb_program_data_dir(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, cch, L"%s\\AppSandbox", base);
    CreateDirectoryW(out, NULL);
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

#endif
