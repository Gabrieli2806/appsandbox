/*
 * vm_clipboard.c -- Per-VM clipboard synchronization over Hyper-V sockets.
 *
 * Extracted from vm_display_idd.c. Manages two channels:
 *   :0005  Host->guest (writer) — sends FORMAT_LIST, serves DATA_REQ
 *   :0006  Guest->host (reader) — receives FORMAT_LIST, fetches data
 *
 * Focus-gated: sync is disabled by default. The display window toggles
 * it on focus/blur. When disabled, no clipboard traffic flows in either
 * direction, which prevents background VMs from corrupting the host
 * clipboard through format round-trip garbling.
 */

#include "vm_clipboard.h"
#include "hcs_vm.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

/* ---- HV socket address ---- */

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

#pragma pack(push, 1)
typedef struct {
    unsigned short Family;
    unsigned short Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;
#pragma pack(pop)

/* Both superseded by hcs_service_guid(os_type, 5|6, ...) — kept for grep.
   Windows VMs reach byte-identical GUIDs via the helper. */
static const GUID CLIPBOARD_SERVICE_GUID =
    { 0xa5b0cafe, 0x0005, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static const GUID CLIPBOARD_READER_SERVICE_GUID =
    { 0xa5b0cafe, 0x0006, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Wire protocol ---- */

#define CLIP_MAGIC          0x504C4341u
#define CLIP_READY_MAGIC    0x59444C43u

#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
/* CLIP_MSG_SYNC_ENABLE = 12 (defined in header) */

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)
#define CLIP_FILE_CHUNK     (1024 * 1024)

/* Limits on what an (untrusted) guest can make the host do.
   - All formats of one copy from the guest together: without this one
     FORMAT_LIST could make the host hold CLIP_MAX_FORMATS x CLIP_MAX_PAYLOAD
     (4 GB). Two full payloads keep a 4K screenshot's CF_DIB + CF_DIBV5.
   - Clipboard changes the guest may announce per second: each one rewrites
     the host clipboard and wakes every clipboard listener on the host.
   - Data requests from the guest per second: each one opens the host
     clipboard, which other host apps then find busy. A Windows guest asks
     for every offered format (at most CLIP_MAX_FORMATS) once per push. */
#define CLIP_MAX_COPY_BYTES          (2u * CLIP_MAX_PAYLOAD)
#define CLIP_MAX_GUEST_LISTS_PER_SEC 10u
#define CLIP_MAX_DATA_REQS_PER_SEC   256u

#define WM_CLIP_READER_APPLY (WM_APP + 11)

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;

typedef struct {
    UINT32 path_len;
    UINT64 file_size;
    UINT8  is_directory;
} ClipFileInfo;
#pragma pack(pop)

typedef struct {
    UINT   local_id;
    UINT   remote_id;
    BYTE  *data;
    UINT32 data_size;
    BOOL   is_hdrop;
} ClipFmtEntry;

/* ---- Instance data ---- */

struct VmClipboardData {
    GUID             runtime_id;
    wchar_t          os_type[32];  /* for picking the HV-socket service GUID variant */
    HWND             hwnd;
    volatile BOOL    stop;
    volatile BOOL    sync_enabled;
    volatile BOOL    share_host_to_guest;  /* default TRUE */
    volatile BOOL    share_guest_to_host;  /* default TRUE */

    VmClipboardLogFn log_fn;
    void            *log_ud;

    /* Writer channel (:0005 host->guest) */
    volatile SOCKET  writer_socket;
    volatile LONG    writer_suppress;
    HANDLE           writer_thread;
    CRITICAL_SECTION writer_cs;

    /* Push worker: every host->guest clipboard send happens on this thread so
       the UI thread never blocks on the socket. The socket is busy for the whole
       of a file transfer (the guest expects one un-interrupted byte stream), so
       a send from the UI thread would freeze the window for the length of the
       transfer - exactly when the user is waiting for a big file to arrive. */
    HANDLE           push_thread;
    HANDLE           push_event;      /* signalled when work is queued */
    volatile LONG    pending_sync;    /* -1 = nothing, 0 = disable, 1 = enable */
    volatile LONG    pending_push;    /* 1 = a FORMAT_LIST push is queued */

    /* GetClipboardSequenceNumber() of the clipboard content last pushed to the
       guest. An unchanged value means the guest already holds this content, so
       pushing it again would only make the guest delete its staged file and
       download it all over again. */
    volatile LONG    pushed_seq;
    volatile LONG    pushed_seq_valid;

    /* Formats of the last FORMAT_LIST pushed to the guest (under writer_cs).
       The guest is served only these: it never learns about the others, such
       as the full host paths in "FileNameW" next to a CF_HDROP. */
    UINT             offered_fmts[CLIP_MAX_FORMATS];
    UINT             offered_count;

    /* Rate window for data requests from the guest (writer thread only). */
    ULONGLONG        req_window;
    UINT             req_count;

    /* Reader channel (:0006 guest->host) */
    volatile SOCKET  reader_socket;
    volatile LONG    reader_suppress;
    HANDLE           reader_thread;
    CRITICAL_SECTION reader_cs;
    ClipFmtEntry     reader_fmts[CLIP_MAX_FORMATS];
    int              reader_fmt_count;
    int              reader_fetch_idx;

    /* Per-copy limits for the copy being fetched from the guest, and the rate
       window for the guest's FORMAT_LISTs (reader thread only). */
    UINT64           copy_bytes_left;      /* format data still accepted in this copy */
    BOOL             copy_drop_logged;     /* one log line per copy about dropped data */
    ULONGLONG        list_window;
    UINT             list_count;

    /* Folder for files copied in the guest (%ProgramData%\AppSandbox\clip\<vm>),
       empty when no safe folder could be set up (guest files are then
       refused). root_handle keeps the admin-only clip root in place. */
    wchar_t          temp_dir[MAX_PATH];
    HANDLE           root_handle;
    UINT64           recv_budget;          /* bytes still accepted in this guest->host transfer */
    UINT32           recv_entries;         /* files + folders still accepted in it */
};

/* GetClipboardSequenceNumber() right after any VM in this process last put
   guest content on the host clipboard. Content with this number came from a
   guest, whichever window owns the clipboard now. */
static volatile LONG g_guest_content_seq;

/* ---- Logging ---- */

static void clip_log(VmClipboard clip, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!clip || !clip->log_fn) return;
    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);
    clip->log_fn(buf, clip->log_ud);
}

/* ---- Low-level I/O ---- */

static BOOL clip_recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

static BOOL clip_send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* Read and drop n bytes, so the stream stays aligned on message headers. */
static BOOL clip_recv_discard(SOCKET s, UINT64 n)
{
    BYTE skip[4096];
    while (n > 0) {
        int c = n > sizeof(skip) ? (int)sizeof(skip) : (int)n;
        if (!clip_recv_exact(s, skip, c)) return FALSE;
        n -= (UINT64)c;
    }
    return TRUE;
}

/* Fixed one-second window. Returns 1 while fewer than max_per_sec events were
   seen in the current window, -1 for the first event over the limit (the
   caller logs once) and 0 for the rest. Each window is owned by one thread. */
static int clip_rate_check(ULONGLONG *window_start, UINT *count, UINT max_per_sec)
{
    ULONGLONG now = GetTickCount64();
    if (now - *window_start >= 1000) {
        *window_start = now;
        *count = 0;
    }
    if (*count < max_per_sec) { (*count)++; return 1; }
    if (*count == max_per_sec) { (*count)++; return -1; }
    return 0;
}

static SOCKET clip_connect_hv(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family    = AF_HYPERV;
    addr.VmId      = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    return s;
}

/* ---- Utility ---- */

static BOOL clip_should_skip(UINT fmt)
{
    switch (fmt) {
    case 0:
    case CF_BITMAP:
    case CF_PALETTE:
    case CF_OWNERDISPLAY:
    case CF_METAFILEPICT:
    case CF_ENHMETAFILE:
    case CF_DSPTEXT:
    case CF_DSPBITMAP:
    case CF_DSPMETAFILEPICT:
    case CF_DSPENHMETAFILE:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Formats the host accepts from a guest; everything else is dropped. Shell
   formats can name host files ("FileNameW", "Shell IDList Array", raw
   CF_HDROP bytes, with "Preferred DropEffect" even a move on paste), and OLE
   formats ("Ole Private Data", "Link Source", ...) can start COM objects on
   paste. Only these fixed names are ever registered, so a guest cannot fill
   the session's atom table with clipboard format names. CF_HDROP is only
   the end marker of a file transfer: the host builds the file list itself,
   from its own temp folder. */
static const UINT clip_guest_std_formats[] = {
    CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT, CF_LOCALE, CF_DIB, CF_DIBV5, CF_HDROP
};
static const char *const clip_guest_named_formats[] = {
    "HTML Format", "Rich Text Format", "PNG"
};

/* Host format id for a format the guest announced, or 0 to drop it.
   name: the registered name the guest sent, or NULL when it sent none (a
   predefined CF_* id, or - from a Linux guest - a registered id the host
   itself announced earlier, such as its "HTML Format"). */
static UINT clip_guest_format_to_local(UINT32 fmt_id, const char *name)
{
    char host_name[256];
    size_t i;

    if (name) {
        for (i = 0; i < ARRAYSIZE(clip_guest_named_formats); i++)
            if (_stricmp(name, clip_guest_named_formats[i]) == 0)
                return RegisterClipboardFormatA(clip_guest_named_formats[i]);
        return 0;
    }
    for (i = 0; i < ARRAYSIZE(clip_guest_std_formats); i++)
        if (fmt_id == clip_guest_std_formats[i]) return fmt_id;
    if (fmt_id >= 0xC000 && fmt_id <= 0xFFFF &&
        GetClipboardFormatNameA(fmt_id, host_name, sizeof(host_name)) > 0) {
        for (i = 0; i < ARRAYSIZE(clip_guest_named_formats); i++)
            if (_stricmp(host_name, clip_guest_named_formats[i]) == 0) return fmt_id;
    }
    return 0;
}

/* dst = a + "\" + b. FALSE (dst untouched) when that would not fit in
   MAX_PATH with room left for the "\*" of a directory search: the secure CRT
   ends the process on an over-long path instead of truncating it. */
static BOOL clip_join_path(wchar_t *dst, const wchar_t *a, const wchar_t *b)
{
    if (wcslen(a) + 1 + wcslen(b) + 2 >= MAX_PATH) return FALSE;
    swprintf_s(dst, MAX_PATH, L"%s\\%s", a, b);
    return TRUE;
}

/* Delete dir and everything in it. Links (junctions, symlinks) are removed
   as links and never followed; when dir itself is a link, or is not set,
   nothing is touched. */
static void clip_remove_dir_recursive(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    DWORD attrs;

    if (!dir || !dir[0]) return;
    attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (!clip_join_path(pattern, dir, L"*")) return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!clip_join_path(path, dir, fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            /* A junction/link: remove the link itself, never its target. */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path);
            else DeleteFileW(path);
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            clip_remove_dir_recursive(path);
        else
            DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

/* ---- Clipboard folders ----

   Files copied in a guest land in %ProgramData%\AppSandbox\clip\<vm>\. The
   app runs elevated, so that folder must not be one that a non-elevated
   process of the same user can change. (In %TEMP% such a process could swap
   the folder for a junction and make the elevated app delete, write or read
   files wherever the junction points.) The clip root is owned by
   Administrators with a protected DACL: SYSTEM and Administrators full
   control, Users read/execute so Explorer can paste the files. Every VM keeps
   a handle open on the root, without delete sharing, so it cannot be renamed
   or replaced while the VM runs. */

#define CLIP_ROOT_SDDL L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"

/* %ProgramData%\AppSandbox\clip, taken from the known-folder settings
   rather than the ProgramData environment variable. FALSE when unknown or
   too long. */
static BOOL clip_root_path(wchar_t *out)
{
    PWSTR pd = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &pd)) && pd &&
        pd[0] && wcslen(pd) + 32 < MAX_PATH) {
        swprintf_s(out, MAX_PATH, L"%s\\AppSandbox\\clip", pd);
        ok = TRUE;
    }
    if (pd) CoTaskMemFree(pd);
    return ok;
}

/* Open the folder dir itself (never a link's target) for checking and
   setting its permissions. The handle shares read and write but not delete,
   so while it is open nobody can rename, delete or replace the folder.
   INVALID_HANDLE_VALUE when dir is missing, a link, or not a folder. */
static HANDLE clip_open_dir_pinned(const wchar_t *dir)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileW(dir, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* TRUE when SYSTEM or Administrators owns the object. A non-elevated
   process cannot create an object owned by either. */
static BOOL clip_owner_is_admin(HANDLE h)
{
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    BOOL ok = FALSE;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, NULL, NULL, NULL, &sd) == ERROR_SUCCESS && owner)
        ok = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) ||
             IsWellKnownSid(owner, WinLocalSystemSid);
    if (sd) LocalFree(sd);
    return ok;
}

/* Set up the clip root (root receives its path) and return a pinned handle
   to it, or INVALID_HANDLE_VALUE when no safe root is available. A root that
   an administrator did not create (for example one a non-elevated process
   made first, or a link) is removed and created afresh. */
static HANDLE clip_open_root(wchar_t *root)
{
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    HANDLE h = INVALID_HANDLE_VALUE;
    wchar_t parent[MAX_PATH], *slash;
    int attempt;

    root[0] = L'\0';
    if (!clip_root_path(root)) return INVALID_HANDLE_VALUE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(CLIP_ROOT_SDDL, SDDL_REVISION_1,
                                                              &sd, NULL))
        return INVALID_HANDLE_VALUE;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) {
        LocalFree(sd);
        return INVALID_HANDLE_VALUE;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    /* %ProgramData%\AppSandbox, as the rest of the app creates it. */
    wcscpy_s(parent, MAX_PATH, root);
    slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = L'\0';
        CreateDirectoryW(parent, NULL);
    }

    for (attempt = 0; attempt < 2 && h == INVALID_HANDLE_VALUE; attempt++) {
        BOOL created = CreateDirectoryW(root, &sa);
        BOOL foreign = FALSE;
        DWORD attrs;
        if (!created && GetLastError() != ERROR_ALREADY_EXISTS) break;
        h = clip_open_dir_pinned(root);
        if (h != INVALID_HANDLE_VALUE && !created) {
            /* Made by an earlier run: keep it only if an administrator owns
               it, and put its permissions back to the expected ones. */
            if (!clip_owner_is_admin(h)) {
                foreign = TRUE;
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            } else if (SetSecurityInfo(h, SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                       NULL, NULL, dacl, NULL) != ERROR_SUCCESS) {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            }
        }
        if (h != INVALID_HANDLE_VALUE || attempt > 0) continue;
        /* A link, a file, or a folder a non-administrator made: remove it
           (a link only as the link) and create it afresh. An administrator's
           folder that merely could not be opened is left alone: other VMs
           may be using it. */
        attrs = GetFileAttributesW(root);
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
            DeleteFileW(root);
        else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            RemoveDirectoryW(root);
        else if (foreign)
            clip_remove_dir_recursive(root);
        else
            break;
    }
    LocalFree(sd);
    return h;
}

/* Create the folders above file_path. Returns how many it had to create. */
static UINT32 clip_ensure_parent_dir(const wchar_t *file_path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *p;
    UINT32 made = 0;
    wcscpy_s(dir, MAX_PATH, file_path);
    p = wcsrchr(dir, L'\\');
    if (!p) return 0;
    *p = L'\0';
    for (p = dir; *p; p++) {
        if (*p == L'\\' && p > dir && *(p - 1) != L':') {
            *p = L'\0';
            if (CreateDirectoryW(dir, NULL)) made++;
            *p = L'\\';
        }
    }
    if (CreateDirectoryW(dir, NULL)) made++;
    return made;
}

/* A relative path from the guest: plain components only. No "..", no
   drive or stream syntax (':'), no wildcards or other characters Windows
   reserves, no trailing dots/spaces, no device names (CON, NUL, COM1...). */
static BOOL clip_component_is_safe(const wchar_t *c, size_t n)
{
    static const wchar_t *const reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    size_t i, base = n;
    if (n == 0 || n > 255) return FALSE;
    if ((n == 1 && c[0] == L'.') || (n == 2 && c[0] == L'.' && c[1] == L'.')) return FALSE;
    if (c[n - 1] == L'.' || c[n - 1] == L' ') return FALSE;
    for (i = 0; i < n; i++) {
        if (c[i] < 32 || wcschr(L"<>:\"/|?*", c[i])) return FALSE;
        if (c[i] == L'.' && base == n) base = i;
    }
    for (i = 0; i < ARRAYSIZE(reserved); i++)
        if (wcslen(reserved[i]) == base && _wcsnicmp(c, reserved[i], base) == 0) return FALSE;
    return TRUE;
}

static BOOL clip_path_is_safe(const wchar_t *rel_path)
{
    const wchar_t *start = rel_path, *p;
    if (!rel_path[0] || rel_path[0] == L'\\') return FALSE;
    if (wcslen(rel_path) > MAX_PATH - 50) return FALSE;
    for (p = rel_path; ; p++) {
        if (*p == L'\\' || *p == L'\0') {
            if (!clip_component_is_safe(start, (size_t)(p - start))) return FALSE;
            if (*p == L'\0') break;
            start = p + 1;
        }
    }
    return TRUE;
}

/* Most a guest may send in one copy (all files together), in bytes and in
   files + folders (empty folders and files cost no bytes but still fill the
   disk's file table). */
#define CLIP_MAX_TRANSFER_BYTES   (16ULL * 1024 * 1024 * 1024)
#define CLIP_MAX_TRANSFER_ENTRIES 250000u

/* TRUE when path names an entry inside one of this app's clipboard folders
   (%ProgramData%\AppSandbox\clip\<vm>\<name>...), where every CF_HDROP
   this process builds from guest files points. */
static BOOL clip_path_in_clip_temp(const wchar_t *path)
{
    wchar_t root[MAX_PATH], full[MAX_PATH];
    const wchar_t *rest, *sep;
    size_t root_len;
    DWORD n;

    if (!clip_root_path(root)) return FALSE;
    root_len = wcslen(root);
    if (root_len + 2 > MAX_PATH) return FALSE;
    root[root_len++] = L'\\';
    root[root_len] = L'\0';

    /* GetFullPathNameW folds "..", so a path cannot climb back out. */
    n = GetFullPathNameW(path, MAX_PATH, full, NULL);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    if (_wcsnicmp(full, root, root_len) != 0) return FALSE;
    rest = full + root_len;
    sep = wcschr(rest, L'\\');
    return sep && sep > rest && sep[1] != L'\0';
}

/* TRUE when the CF_HDROP now on the host clipboard was put there by this
   process, i.e. it lists files a guest copied, not files the host user
   copied. Call with the clipboard open. */
static BOOL clip_hdrop_is_from_guest(void)
{
    HWND owner = GetClipboardOwner();
    DWORD pid = 0;
    LONG guest_seq = InterlockedCompareExchange(&g_guest_content_seq, 0, 0);
    DWORD seq = GetClipboardSequenceNumber();

    if (guest_seq != 0 && seq != 0 && (LONG)seq == guest_seq) return TRUE;
    if (owner) GetWindowThreadProcessId(owner, &pid);
    return pid != 0 && pid == GetCurrentProcessId();
}

/* ---- File transfer: host->guest (send files from host clipboard) ---- */

static BOOL clip_send_file_entry(SOCKET s, const wchar_t *full_path, const wchar_t *rel_path)
{
    ClipHeader hdr;
    ClipFileInfo fi;
    DWORD attrs = GetFileAttributesW(full_path);
    UINT32 path_bytes;

    if (attrs == INVALID_FILE_ATTRIBUTES) return TRUE;

    path_bytes = (UINT32)(wcslen(rel_path) * sizeof(wchar_t));
    fi.path_len = path_bytes;

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        wchar_t search[MAX_PATH];
        HANDLE h;

        fi.file_size = 0;
        fi.is_directory = 1;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr))) return FALSE;
        if (!clip_send_all(s, &fi, sizeof(fi))) return FALSE;
        if (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes)) return FALSE;

        h = clip_join_path(search, full_path, L"*") ? FindFirstFileW(search, &fd)
                                                    : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                /* Links inside a copied folder (symlinks, junctions) are not
                   followed: they can point anywhere on the host, outside
                   what the user copied (a guest can plant one through a
                   shared folder), and a junction loop never ends. Other
                   reparse points, such as cloud placeholders, are files. */
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    IsReparseTagNameSurrogate(fd.dwReserved0)) continue;
                /* Paths too long for MAX_PATH are skipped, not truncated. */
                if (!clip_join_path(child_full, full_path, fd.cFileName) ||
                    !clip_join_path(child_rel, rel_path, fd.cFileName)) continue;
                if (!clip_send_file_entry(s, child_full, child_rel)) {
                    FindClose(h);
                    return FALSE;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        HANDLE hFile = CreateFileW(full_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER size;
        BYTE *chunk;
        UINT64 remaining;

        if (hFile == INVALID_HANDLE_VALUE) return TRUE;

        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        if (!chunk) { CloseHandle(hFile); return FALSE; }

        GetFileSizeEx(hFile, &size);
        fi.file_size = (UINT64)size.QuadPart;
        fi.is_directory = 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr)) ||
            !clip_send_all(s, &fi, sizeof(fi)) ||
            (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes))) {
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
            return FALSE;
        }

        remaining = fi.file_size;
        while (remaining > 0) {
            DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
            DWORD bytes_read;
            if (!ReadFile(hFile, chunk, to_read, &bytes_read, NULL) || bytes_read == 0) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            if (!clip_send_all(s, chunk, (int)bytes_read)) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            remaining -= bytes_read;
        }
        HeapFree(GetProcessHeap(), 0, chunk);
        CloseHandle(hFile);
    }
    return TRUE;
}

/* ---- HDROP builder (reader: guest->host file transfer) ---- */

static HGLOBAL clip_build_hdrop_from_temp(const wchar_t *temp_dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int count = 0;
    SIZE_T total_size;
    HGLOBAL hMem;
    DROPFILES *df;
    wchar_t *ptr;
    int i;

    if (!temp_dir[0]) return NULL;
    if (!clip_join_path(pattern, temp_dir, L"*")) return NULL;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (!clip_join_path(paths[count], temp_dir, fd.cFileName)) continue;
        count++;
        if (count >= CLIP_MAX_FORMATS) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (count == 0) return NULL;

    total_size = sizeof(DROPFILES);
    for (i = 0; i < count; i++)
        total_size += (wcslen(paths[i]) + 1) * sizeof(wchar_t);
    total_size += sizeof(wchar_t);

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!hMem) return NULL;

    df = (DROPFILES *)GlobalLock(hMem);
    if (!df) { GlobalFree(hMem); return NULL; }

    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;

    ptr = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    for (i = 0; i < count; i++) {
        /* destsz must be this entry's reserved slice (wcslen+1), NOT MAX_PATH:
           the buffer is sized tightly per path (see total_size above), so passing
           MAX_PATH makes wcscpy_s's secure-CRT tail-fill memset run past the
           allocation -> heap overflow (caught by full page heap at memset). */
        wcscpy_s(ptr, wcslen(paths[i]) + 1, paths[i]);
        ptr += wcslen(paths[i]) + 1;
    }
    *ptr = L'\0';

    GlobalUnlock(hMem);
    return hMem;
}

/* ---- Push queue ----
 *
 * The UI thread never talks to the clipboard socket directly: it queues the
 * request here and the push worker performs the send. The worker can afford
 * to wait for the socket (it is a background thread); the UI thread cannot,
 * because the socket is held for the whole of a file transfer.
 */

static void clip_queue_sync_enable(VmClipboard clip, BOOL enabled)
{
    InterlockedExchange(&clip->pending_sync, enabled ? 1 : 0);
    if (clip->push_event) SetEvent(clip->push_event);
}

static void clip_queue_push(VmClipboard clip)
{
    InterlockedExchange(&clip->pending_push, 1);
    if (clip->push_event) SetEvent(clip->push_event);
}

/* ---- Writer channel: send SYNC_ENABLE ---- */

static void clip_send_sync_enable(VmClipboard clip, BOOL enabled)
{
    ClipHeader hdr;
    UINT8 flag;
    SOCKET s = clip->writer_socket;
    if (s == INVALID_SOCKET) return;

    flag = enabled ? 1 : 0;
    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_SYNC_ENABLE;
    hdr.format = 0;
    hdr.data_size = 1;

    EnterCriticalSection(&clip->writer_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    clip_send_all(s, &flag, 1);
    LeaveCriticalSection(&clip->writer_cs);

    clip_log(clip, L"CLIP: Sent SYNC_ENABLE(%d) to guest.", (int)flag);
}

/* ---- Writer channel: send FORMAT_LIST ---- */

static void clip_send_format_list(VmClipboard clip)
{
    ClipHeader hdr;
    SOCKET s;
    UINT fmt;
    BYTE buf[16384];
    UINT sent_fmts[CLIP_MAX_FORMATS];
    int offset = 4;
    UINT32 count = 0;
    DWORD seq;

    s = clip->writer_socket;
    if (s == INVALID_SOCKET) return;

    /* GetClipboardSequenceNumber() changes every time the clipboard content
       changes. When it has not changed since the last push, the guest already
       holds this content - including the file it staged in its temp folder -
       so pushing it again would make the guest delete the staged file and
       download it all over again. That happened on every alt-tab back into
       the VM: focus regains re-pushed the clipboard, the guest wiped its
       temp dir on the format list and re-downloaded the file from scratch. */
    seq = GetClipboardSequenceNumber();
    if (seq != 0 &&
        InterlockedCompareExchange(&clip->pushed_seq_valid, 0, 0) &&
        (LONG)seq == InterlockedCompareExchange(&clip->pushed_seq, 0, 0)) {
        clip_log(clip, L"CLIP: Clipboard unchanged (seq %u) - not re-pushing to guest.",
                 (unsigned)seq);
        return;
    }

    /* Another clipboard owner can hold the clipboard open for a moment (the
       UI thread applies guest content on the same clipboard). Retry briefly
       rather than dropping the push - the guest would not hear about this
       content until the next clipboard change. */
    {
        int attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            if (OpenClipboard(clip->hwnd)) break;
            Sleep(20);
        }
        if (attempt == 10) return;
    }

    {
        BOOL has_hdrop = IsClipboardFormatAvailable(CF_HDROP);
        fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0 && count < CLIP_MAX_FORMATS) {
            char name[256];
            int name_len = 0;

            if (clip_should_skip(fmt)) continue;
            if (has_hdrop && fmt != CF_HDROP) continue;

            if (fmt >= 0xC000)
                name_len = GetClipboardFormatNameA(fmt, name, sizeof(name));

            if (offset + 8 + name_len > 16384) break;

            *(UINT32 *)(buf + offset) = fmt;       offset += 4;
            *(UINT32 *)(buf + offset) = (UINT32)name_len; offset += 4;
            if (name_len > 0) {
                memcpy(buf + offset, name, name_len);
                offset += name_len;
            }
            sent_fmts[count] = fmt;
            count++;
        }
    }

    /* Re-read the sequence number now that we hold the clipboard: it now
       describes exactly the content we are about to push. Reading it before
       opening the clipboard left a window in which another application could
       have replaced the content, which would make us record a sequence
       number that does not match what we sent. */
    {
        DWORD cur = GetClipboardSequenceNumber();
        if (cur != 0) seq = cur;
    }

    CloseClipboard();

    if (count == 0) return;

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)offset;

    InterlockedExchange(&clip->reader_suppress, 1);

    EnterCriticalSection(&clip->writer_cs);
    memcpy(clip->offered_fmts, sent_fmts, count * sizeof(UINT));
    clip->offered_count = count;
    clip_send_all(s, &hdr, sizeof(hdr));
    clip_send_all(s, buf, offset);
    LeaveCriticalSection(&clip->writer_cs);

    /* Remember what we pushed so the next focus change does not push it again. */
    InterlockedExchange(&clip->pushed_seq, (LONG)seq);
    InterlockedExchange(&clip->pushed_seq_valid, seq != 0);

    clip_log(clip, L"CLIP: Sent format list (%u formats) to guest.", count);
}

/* ---- Push worker thread ---- */

static DWORD WINAPI clip_push_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;

    for (;;) {
        LONG sync;
        LONG push;

        WaitForSingleObject(clip->push_event, 250);
        if (clip->stop) break;

        /* Drain the queue. SYNC_ENABLE is always sent before the format list
           that depends on it, so the guest never sees a format list while
           its sync is still disabled. */
        for (;;) {
            sync = InterlockedExchange(&clip->pending_sync, -1);
            push = InterlockedExchange(&clip->pending_push, 0);
            if (sync < 0 && !push) break;

            if (sync >= 0) clip_send_sync_enable(clip, sync != 0);
            if (push)     clip_send_format_list(clip);
        }
    }

    clip->push_thread = NULL;
    return 0;
}

/* ---- Writer channel: handle DATA_REQ from guest ---- */

static void clip_send_empty_response(VmClipboard clip, SOCKET s, UINT fmt)
{
    ClipHeader hdr;
    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
    hdr.format = fmt;
    hdr.data_size = 0;
    EnterCriticalSection(&clip->writer_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&clip->writer_cs);
}

static void clip_handle_data_request(VmClipboard clip, UINT fmt)
{
    ClipHeader hdr;
    SOCKET s = clip->writer_socket;
    BOOL offered = FALSE;
    UINT i;
    int rate;

    if (s == INVALID_SOCKET) return;

    /* Every request is answered (the guest waits for the response), but
       refusals answer empty. When host->guest sharing is off (or the window
       is unfocused), return empty — don't leak host clipboard to the guest. */
    if (!clip->sync_enabled || !clip->share_host_to_guest) {
        clip_send_empty_response(clip, s, fmt);
        return;
    }

    rate = clip_rate_check(&clip->req_window, &clip->req_count, CLIP_MAX_DATA_REQS_PER_SEC);
    if (rate <= 0) {
        if (rate < 0)
            clip_log(clip, L"CLIP: Too many clipboard requests from the guest - answering them empty for a moment.");
        clip_send_empty_response(clip, s, fmt);
        return;
    }

    /* Serve only formats the host offered in its last FORMAT_LIST. */
    EnterCriticalSection(&clip->writer_cs);
    for (i = 0; i < clip->offered_count; i++) {
        if (clip->offered_fmts[i] == fmt) { offered = TRUE; break; }
    }
    LeaveCriticalSection(&clip->writer_cs);
    if (!offered) {
        clip_send_empty_response(clip, s, fmt);
        return;
    }

    if (fmt == CF_HDROP) {
        wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
        int path_count = 0;
        UINT refused = 0;

        /* Snapshot the dropped paths while the clipboard is open, then release
           it again. Holding OpenClipboard() across a multi-gigabyte transfer
           would lock every other application on the host out of the clipboard
           for the whole transfer. */
        if (OpenClipboard(NULL)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                /* Files go to a guest only when the host user copied them.
                   A file list this process put on the clipboard came from a
                   guest and points into a clipboard folder; any other
                   path in it is never sent. */
                BOOL from_guest = clip_hdrop_is_from_guest();
                if (file_count > CLIP_MAX_FORMATS) file_count = CLIP_MAX_FORMATS;
                for (fi = 0; fi < file_count; fi++) {
                    if (DragQueryFileW(hDrop, fi, paths[path_count], MAX_PATH) == 0) continue;
                    if (from_guest && !clip_path_in_clip_temp(paths[path_count])) {
                        refused++;
                        continue;
                    }
                    path_count++;
                }
            }
            CloseClipboard();
        }

        if (refused > 0)
            clip_log(clip, L"CLIP: Refused to send %u file(s) outside the clipboard folder to the guest.",
                     refused);
        clip_log(clip, L"CLIP: Guest requested the copied files (%d item(s)).", path_count);

        if (path_count == 0) {
            clip_send_empty_response(clip, s, fmt);
            return;
        }

        {
            EnterCriticalSection(&clip->writer_cs);
            for (i = 0; i < (UINT)path_count; i++) {
                const wchar_t *path = paths[i];
                const wchar_t *name;
                name = wcsrchr(path, L'\\');
                name = name ? name + 1 : path;
                clip_send_file_entry(s, path, name);
            }
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
            hdr.format = CF_HDROP;
            hdr.data_size = 0;
            clip_send_all(s, &hdr, sizeof(hdr));
            LeaveCriticalSection(&clip->writer_cs);
        }
        return;
    }

    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(fmt);
        void *ptr = hData ? GlobalLock(hData) : NULL;
        SIZE_T size = ptr ? GlobalSize(hData) : 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = (ptr && size <= CLIP_MAX_PAYLOAD) ? (UINT32)size : 0;

        EnterCriticalSection(&clip->writer_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        if (hdr.data_size > 0)
            clip_send_all(s, ptr, (int)hdr.data_size);
        LeaveCriticalSection(&clip->writer_cs);

        if (ptr) GlobalUnlock(hData);
        CloseClipboard();
    } else {
        clip_send_empty_response(clip, s, fmt);
    }
}

/* ---- Writer recv thread ---- */

static BOOL clip_writer_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {
    case CLIP_MSG_FORMAT_DATA_REQ:
        clip_handle_data_request(clip, hdr->format);
        return TRUE;
    default:
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!clip_recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

static DWORD WINAPI clip_writer_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;
    SOCKET s;
    ClipHeader hdr;

    for (;;) {
        s = clip->writer_socket;

        while (!clip->stop) {
            if (!clip_recv_exact(s, &hdr, sizeof(hdr))) break;
            if (hdr.magic != CLIP_MAGIC) break;
            if (!clip_writer_handle_message(clip, s, &hdr)) break;
        }

        /* Atomically claim the socket so the writer thread and clip_stop()
           never double-close the same handle (App Verifier invalid-handle). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&clip->writer_socket, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET) closesocket(old);
        }

        if (clip->stop) break;

        while (!clip->stop) {
            int wait;
            SOCKET ws;
            for (wait = 0; wait < 3000 && !clip->stop; wait += 500)
                Sleep(500);
            if (clip->stop) break;

            { GUID svc; hcs_service_guid(clip->os_type, 5, &svc);
              ws = clip_connect_hv(&clip->runtime_id, &svc, 2000); }
            if (ws != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(ws, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(ws, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->writer_socket = ws;
                    clip_log(clip, L"CLIP: Writer reconnected (GUID :0005).");
                    /* The guest side restarted, so it lost its clipboard and
                       the file it had staged. Force a fresh push even if the
                       host clipboard has not changed since the last one. */
                    InterlockedExchange(&clip->pushed_seq_valid, 0);
                    if (clip->sync_enabled) {
                        clip_queue_sync_enable(clip, TRUE);
                        if (clip->share_host_to_guest)
                            clip_queue_push(clip);
                    } else {
                        clip_queue_sync_enable(clip, FALSE);
                    }
                    break;
                }
                closesocket(ws);
            }
        }

        if (clip->stop) break;
    }

    clip->writer_thread = NULL;
    return 0;
}

/* ---- Reader channel: file data receive ---- */

/* One log line per copy when part of it is dropped (a guest can send
   thousands of entries; one line each would flood the log). */
static void clip_log_copy_drop(VmClipboard clip, const wchar_t *what)
{
    if (clip->copy_drop_logged) return;
    clip->copy_drop_logged = TRUE;
    clip_log(clip, L"Clipboard: part of a copy from the VM was dropped (%s).", what);
}

/* TRUE while the host is fetching CF_HDROP from the guest: the only time
   the guest's file entries are wanted. */
static BOOL clip_reader_hdrop_pending(VmClipboard clip)
{
    BOOL pending;
    int idx;
    EnterCriticalSection(&clip->reader_cs);
    idx = clip->reader_fetch_idx;
    pending = idx >= 0 && idx < clip->reader_fmt_count &&
              clip->reader_fmts[idx].local_id == CF_HDROP;
    LeaveCriticalSection(&clip->reader_cs);
    return pending;
}

static BOOL clip_reader_recv_file_data(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    ClipFileInfo fi;
    wchar_t rel_path[MAX_PATH], full_path[MAX_PATH];

    if (hdr->data_size < sizeof(fi)) return FALSE;
    if (!clip_recv_exact(s, &fi, sizeof(fi))) return FALSE;

    if (fi.path_len == 0 || fi.path_len > (MAX_PATH - 1) * sizeof(wchar_t)) {
        if (!clip_recv_discard(s, hdr->data_size - (UINT32)sizeof(fi))) return FALSE;
        return clip_recv_discard(s, fi.file_size);
    }
    /* The header covers exactly the entry info and its path. */
    if (hdr->data_size != sizeof(fi) + fi.path_len) return FALSE;

    if (!clip_recv_exact(s, rel_path, (int)fi.path_len)) return FALSE;
    rel_path[fi.path_len / sizeof(wchar_t)] = L'\0';

    /* Accept entries only while the host is fetching the guest's files with
       guest->host sharing on, for a safe relative path that fits under the
       temp folder, within the per-copy budget, and only if the host disk
       keeps 1 GB free; anything else is read off the wire and dropped. */
    {
        BOOL accept = clip->sync_enabled && clip->share_guest_to_host &&
                      clip->temp_dir[0] && clip_reader_hdrop_pending(clip);
        ULARGE_INTEGER free_bytes;
        if (accept && (!clip_path_is_safe(rel_path) ||
                       !clip_join_path(full_path, clip->temp_dir, rel_path))) {
            accept = FALSE;
            clip_log_copy_drop(clip, L"unsafe or too long file name");
        }
        if (accept && clip->recv_entries == 0) {
            accept = FALSE;
            clip_log_copy_drop(clip, L"too many files");
        }
        if (accept && fi.is_directory && fi.file_size != 0) {
            accept = FALSE;   /* a folder carries no data */
        }
        if (accept && !fi.is_directory) {
            if (fi.file_size > clip->recv_budget) {
                accept = FALSE;
                clip_log_copy_drop(clip, L"copy exceeds the size limit");
            } else if (GetDiskFreeSpaceExW(clip->temp_dir, &free_bytes, NULL, NULL) &&
                       free_bytes.QuadPart < fi.file_size + 1024ULL * 1024 * 1024) {
                accept = FALSE;
                clip_log_copy_drop(clip, L"not enough free disk space");
            } else {
                clip->recv_budget -= fi.file_size;
            }
        }
        if (!accept)
            return clip_recv_discard(s, fi.file_size);
        clip->recv_entries--;
    }

    /* Folders the entry's path needs and the guest did not send first also
       count against the per-copy entry limit. */
    {
        UINT32 made = clip_ensure_parent_dir(full_path);
        clip->recv_entries -= made < clip->recv_entries ? made : clip->recv_entries;
    }

    if (fi.is_directory) {
        CreateDirectoryW(full_path, NULL);
    } else {
        HANDLE hFile;
        hFile = CreateFileW(full_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT64 remaining = fi.file_size;
            BOOL write_ok = TRUE;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            if (!chunk) { CloseHandle(hFile); return FALSE; }
            while (remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD written = 0;
                if (!clip_recv_exact(s, chunk, (int)to_read)) {
                    HeapFree(GetProcessHeap(), 0, chunk);
                    CloseHandle(hFile);
                    DeleteFileW(full_path);
                    return FALSE;
                }
                /* Keep reading the stream after a write error (disk full),
                   but stop writing and drop the partial file. */
                if (write_ok && (!WriteFile(hFile, chunk, to_read, &written, NULL) || written != to_read))
                    write_ok = FALSE;
                remaining -= to_read;
            }
            if (!write_ok) {
                CloseHandle(hFile);
                DeleteFileW(full_path);
                clip_log_copy_drop(clip, L"could not write a file, disk full?");
                HeapFree(GetProcessHeap(), 0, chunk);
                return TRUE;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
        } else {
            UINT64 remaining = fi.file_size;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            if (!chunk) return FALSE;
            while (remaining > 0) {
                int c = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (int)remaining;
                if (!clip_recv_exact(s, chunk, c)) { HeapFree(GetProcessHeap(), 0, chunk); return FALSE; }
                remaining -= c;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
        }
    }
    return TRUE;
}

/* ---- Reader channel: request next format or apply ---- */

static void clip_reader_free_pending(VmClipboard clip)
{
    int i;
    for (i = 0; i < clip->reader_fmt_count; i++) {
        if (clip->reader_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, clip->reader_fmts[i].data);
            clip->reader_fmts[i].data = NULL;
        }
    }
}

static void clip_reader_request_next(VmClipboard clip, SOCKET s)
{
    ClipHeader hdr;

    if (clip->reader_fetch_idx >= clip->reader_fmt_count) {
        if (clip->reader_fmt_count > 0 && clip->hwnd && IsWindow(clip->hwnd))
            PostMessageW(clip->hwnd, WM_CLIP_READER_APPLY, 0, 0);
        return;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = clip->reader_fmts[clip->reader_fetch_idx].remote_id;
    hdr.data_size = 0;

    EnterCriticalSection(&clip->reader_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&clip->reader_cs);
}

/* ---- Reader channel: handle messages ---- */

static BOOL clip_reader_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_FORMAT_LIST: {
        BYTE *buf;
        UINT32 count, i;
        int off, rate, announced = 0;

        /* Need at least the 4-byte leading count; reject short frames so the
           `*(UINT32 *)buf` read below cannot over-read the allocation. */
        if (hdr->data_size < 4 || hdr->data_size > 16384) return FALSE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return FALSE;
        if (!clip_recv_exact(s, buf, (int)hdr->data_size)) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }

        /* Echo suppression */
        if (InterlockedExchange(&clip->reader_suppress, 0)) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST suppressed (echo from host->guest).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        /* A guest announcing clipboard changes in a tight loop would keep the
           host clipboard, every clipboard listener on the host and this log
           busy. Lists over the limit are dropped. */
        rate = clip_rate_check(&clip->list_window, &clip->list_count, CLIP_MAX_GUEST_LISTS_PER_SEC);
        if (rate <= 0) {
            if (rate < 0)
                clip_log(clip, L"CLIP-R: Too many clipboard changes from the guest - ignoring them for a moment.");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        /* Focus/direction gate: discard if sync is off or guest->host is off */
        if (!clip->sync_enabled || !clip->share_guest_to_host) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST discarded (sync disabled).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        off = 4;

        EnterCriticalSection(&clip->reader_cs);
        clip_reader_free_pending(clip);
        clip->reader_fmt_count = 0;
        for (i = 0; i < count && off + 8 <= (int)hdr->data_size; i++) {
            UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
            UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
            UINT local_id = 0;
            int j;

            /* off is in [8, data_size] here; reject a name_len that runs past
               the buffer (or whose signed cast would corrupt off) before the
               `off += name_len` advance below. */
            if (name_len > (UINT32)((int)hdr->data_size - off)) break;
            announced++;

            /* Keep only allow-listed formats (see clip_guest_format_to_local);
               a name too long for any of them is dropped. */
            if (name_len == 0) {
                local_id = clip_guest_format_to_local(fmt_id, NULL);
            } else if (name_len < 256) {
                char name[256];
                memcpy(name, buf + off, name_len);
                name[name_len] = '\0';
                local_id = clip_guest_format_to_local(fmt_id, name);
            }
            off += (int)name_len;
            if (local_id == 0) continue;

            /* One slot per host format: a guest repeating one cannot make the
               host fetch it again. */
            for (j = 0; j < clip->reader_fmt_count; j++)
                if (clip->reader_fmts[j].local_id == local_id) break;
            if (j < clip->reader_fmt_count) continue;

            clip->reader_fmts[clip->reader_fmt_count].local_id = local_id;
            clip->reader_fmts[clip->reader_fmt_count].remote_id = fmt_id;
            clip->reader_fmts[clip->reader_fmt_count].data = NULL;
            clip->reader_fmts[clip->reader_fmt_count].data_size = 0;
            clip->reader_fmts[clip->reader_fmt_count].is_hdrop = FALSE;
            clip->reader_fmt_count++;
        }
        clip->reader_fetch_idx = 0;
        /* A new copy from the guest: fresh per-copy limits. */
        clip->copy_bytes_left = CLIP_MAX_COPY_BYTES;
        clip->copy_drop_logged = FALSE;
        clip->recv_budget = CLIP_MAX_TRANSFER_BYTES;
        clip->recv_entries = CLIP_MAX_TRANSFER_ENTRIES;
        LeaveCriticalSection(&clip->reader_cs);

        HeapFree(GetProcessHeap(), 0, buf);

        clip_remove_dir_recursive(clip->temp_dir);
        CreateDirectoryW(clip->temp_dir, NULL);

        clip_log(clip, L"CLIP-R: Received format list (%d of %d formats accepted), fetching data...",
                 clip->reader_fmt_count, announced);
        clip_reader_request_next(clip, s);
        return TRUE;
    }

    case CLIP_MSG_FORMAT_DATA_RESP: {
        int idx;
        UINT local_id = 0;
        BOOL expected, hdrop = FALSE;
        BYTE *data = NULL;

        EnterCriticalSection(&clip->reader_cs);
        idx = clip->reader_fetch_idx;
        expected = idx >= 0 && idx < clip->reader_fmt_count &&
                   hdr->format == clip->reader_fmts[idx].remote_id;
        if (expected) local_id = clip->reader_fmts[idx].local_id;
        LeaveCriticalSection(&clip->reader_cs);

        /* Not the format being fetched (a late answer to a request made
           before the guest's last FORMAT_LIST, or nothing was asked): drop it
           and keep waiting for the answer to the current request. */
        if (!expected)
            return clip_recv_discard(s, hdr->data_size);

        if (local_id == CF_HDROP) {
            /* The files arrived as FILE_DATA; this empty response only ends
               the transfer, and the host builds the file list itself from its
               temp folder. Bytes here would be a guest-made DROPFILES list
               naming arbitrary host paths: never use them. */
            if (hdr->data_size > 0) {
                if (!clip_recv_discard(s, hdr->data_size)) return FALSE;
                clip_log_copy_drop(clip, L"file list with data");
            } else {
                hdrop = TRUE;
            }
        } else if (hdr->data_size > 0) {
            if (hdr->data_size > CLIP_MAX_PAYLOAD || hdr->data_size > clip->copy_bytes_left) {
                if (!clip_recv_discard(s, hdr->data_size)) return FALSE;
                clip_log_copy_drop(clip, L"clipboard data too large");
            } else {
                data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
                if (!data) {
                    if (!clip_recv_discard(s, hdr->data_size)) return FALSE;
                } else if (!clip_recv_exact(s, data, (int)hdr->data_size)) {
                    HeapFree(GetProcessHeap(), 0, data);
                    return FALSE;
                } else {
                    clip->copy_bytes_left -= hdr->data_size;
                }
            }
        }

        EnterCriticalSection(&clip->reader_cs);
        if (idx < clip->reader_fmt_count) {   /* not dropped meanwhile by on_reader_apply */
            clip->reader_fmts[idx].is_hdrop = hdrop;
            if (data) {
                clip->reader_fmts[idx].data = data;
                clip->reader_fmts[idx].data_size = hdr->data_size;
                data = NULL;
            }
            clip->reader_fetch_idx = idx + 1;
        }
        LeaveCriticalSection(&clip->reader_cs);
        if (data) HeapFree(GetProcessHeap(), 0, data);

        clip_reader_request_next(clip, s);
        return TRUE;
    }

    case CLIP_MSG_FILE_DATA:
        CreateDirectoryW(clip->temp_dir, NULL);
        return clip_reader_recv_file_data(clip, s, hdr);

    default:
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!clip_recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

/* ---- Reader recv thread ---- */

static DWORD WINAPI clip_reader_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;
    SOCKET s;
    ClipHeader hdr;

    while (!clip->stop) {
        while (!clip->stop) {
            int wait;
            SOCKET rs;

            if (clip->reader_socket != INVALID_SOCKET) break;

            { GUID svc; hcs_service_guid(clip->os_type, 6, &svc);
              rs = clip_connect_hv(&clip->runtime_id, &svc, 2000); }
            if (rs != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(rs, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->reader_socket = rs;
                    clip_log(clip, L"CLIP-R: Connected (GUID :0006).");
                    break;
                }
                closesocket(rs);
            }

            for (wait = 0; wait < 3000 && !clip->stop; wait += 500)
                Sleep(500);
        }

        if (clip->stop) break;

        s = clip->reader_socket;
        while (!clip->stop) {
            if (!clip_recv_exact(s, &hdr, sizeof(hdr))) break;
            if (hdr.magic != CLIP_MAGIC) break;
            if (!clip_reader_handle_message(clip, s, &hdr)) break;
        }

        EnterCriticalSection(&clip->reader_cs);
        clip_reader_free_pending(clip);
        clip->reader_fmt_count = 0;
        LeaveCriticalSection(&clip->reader_cs);
        clip_remove_dir_recursive(clip->temp_dir);
        /* Atomically claim the socket so the reader thread and clip_stop()
           never double-close the same handle (App Verifier invalid-handle). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&clip->reader_socket, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET) closesocket(old);
        }

        if (clip->stop) break;
    }

    clip->reader_thread = NULL;
    return 0;
}

/* ---- Public API ---- */

ASB_API VmClipboard vm_clipboard_create(const GUID *runtime_id,
                                        const wchar_t *os_type,
                                        HWND hwnd,
                                        VmClipboardLogFn log_fn, void *log_ud)
{
    VmClipboard clip;
    wchar_t root[MAX_PATH];
    SOCKET writer_s;
    GUID svc;

    clip = (VmClipboard)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*clip));
    if (!clip) return NULL;

    clip->runtime_id = *runtime_id;
    if (os_type) wcscpy_s(clip->os_type, 32, os_type);
    clip->hwnd = hwnd;
    clip->log_fn = log_fn;
    clip->log_ud = log_ud;
    clip->stop = FALSE;
    clip->sync_enabled = FALSE;
    clip->share_host_to_guest = TRUE;
    clip->share_guest_to_host = TRUE;
    clip->writer_socket = INVALID_SOCKET;
    clip->reader_socket = INVALID_SOCKET;
    clip->writer_suppress = 0;
    clip->reader_suppress = 0;
    clip->pending_sync = -1;
    clip->pending_push = 0;
    clip->pushed_seq = 0;
    clip->pushed_seq_valid = 0;

    InitializeCriticalSection(&clip->writer_cs);
    InitializeCriticalSection(&clip->reader_cs);

    clip->push_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* One folder per VM under the admin-only clip root: two running VMs
       must not clear each other's transfers. Without a safe root, files
       copied in the guest are refused rather than written elsewhere. */
    clip->root_handle = clip_open_root(root);
    if (clip->root_handle != INVALID_HANDLE_VALUE && wcslen(root) + 16 < MAX_PATH) {
        swprintf_s(clip->temp_dir, MAX_PATH, L"%s\\%08lX%04X", root,
                   runtime_id ? runtime_id->Data1 : GetCurrentProcessId(),
                   runtime_id ? runtime_id->Data2 : 0);
        clip_remove_dir_recursive(clip->temp_dir);   /* left over from a crash */
        CreateDirectoryW(clip->temp_dir, NULL);
    } else {
        clip->temp_dir[0] = L'\0';
        clip_log(clip, L"CLIP: Could not set up the clipboard folder under %s; "
                       L"files copied in the guest will not reach the host.",
                 root[0] ? root : L"ProgramData");
    }

    /* Initial writer connection */
    hcs_service_guid(clip->os_type, 5, &svc);
    writer_s = clip_connect_hv(runtime_id, &svc, 2000);
    if (writer_s != INVALID_SOCKET) {
        UINT32 ready_magic = 0;
        if (clip_recv_exact(writer_s, &ready_magic, sizeof(ready_magic)) &&
            ready_magic == CLIP_READY_MAGIC) {
            DWORD no_timeout = 0;
            setsockopt(writer_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
            clip->writer_socket = writer_s;
        } else {
            closesocket(writer_s);
        }
    }

    clip->push_thread   = CreateThread(NULL, 0, clip_push_thread_proc, clip, 0, NULL);
    clip->writer_thread = CreateThread(NULL, 0, clip_writer_thread_proc, clip, 0, NULL);
    clip->reader_thread = CreateThread(NULL, 0, clip_reader_thread_proc, clip, 0, NULL);

    /* Sync starts out disabled - tell the guest. */
    clip_queue_sync_enable(clip, FALSE);

    return clip;
}

ASB_API void vm_clipboard_destroy(VmClipboard clip)
{
    if (!clip) return;

    clip->stop = TRUE;

    /* Atomically claim each socket so only one side ever closes it; the writer/
       reader threads close their own via the same exchange (no double-close). */
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&clip->writer_socket, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
    }
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&clip->reader_socket, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
    }

    {
        BOOL stuck = FALSE;
        if (clip->writer_thread) {
            if (WaitForSingleObject(clip->writer_thread, 3000) != WAIT_OBJECT_0) stuck = TRUE;
            CloseHandle(clip->writer_thread);
        }
        if (clip->reader_thread) {
            if (WaitForSingleObject(clip->reader_thread, 3000) != WAIT_OBJECT_0) stuck = TRUE;
            CloseHandle(clip->reader_thread);
        }
        /* The push worker waits on writer_cs while the writer thread streams a
           file, so it is joined after the writer thread has released the lock. */
        if (clip->push_event) SetEvent(clip->push_event);
        if (clip->push_thread) {
            if (WaitForSingleObject(clip->push_thread, 3000) != WAIT_OBJECT_0) stuck = TRUE;
            CloseHandle(clip->push_thread);
        }
        if (stuck) {
            /* A thread that is still running would touch freed memory (and
               the viewer's log callback): keep the state instead. */
            OutputDebugStringW(L"vm_clipboard: a thread did not stop; its state is kept\n");
            return;
        }
    }
    if (clip->push_event) {
        CloseHandle(clip->push_event);
        clip->push_event = NULL;
    }

    clip_reader_free_pending(clip);
    clip_remove_dir_recursive(clip->temp_dir);
    if (clip->root_handle && clip->root_handle != INVALID_HANDLE_VALUE)
        CloseHandle(clip->root_handle);

    DeleteCriticalSection(&clip->writer_cs);
    DeleteCriticalSection(&clip->reader_cs);

    HeapFree(GetProcessHeap(), 0, clip);
}

ASB_API void vm_clipboard_set_sync_enabled(VmClipboard clip, BOOL enabled)
{
    if (!clip) return;
    if ((BOOL)clip->sync_enabled == enabled) return;

    clip->sync_enabled = enabled;
    clip_log(clip, L"CLIP: Window %s — clipboard sync %s.",
             enabled ? L"focused" : L"unfocused",
             enabled ? L"ON" : L"OFF");

    /* Queue rather than send: the UI thread must not block on the clipboard
       socket, which is held for the whole of a file transfer. SYNC_ENABLE is
       queued before the format list so the guest sees them in that order. */
    clip_queue_sync_enable(clip, enabled);

    if (enabled && clip->share_host_to_guest) {
        clip_queue_push(clip);
    }
}

ASB_API void vm_clipboard_set_share_directions(VmClipboard clip,
                                               BOOL share_host_to_guest,
                                               BOOL share_guest_to_host)
{
    if (!clip) return;
    clip->share_host_to_guest = share_host_to_guest ? TRUE : FALSE;
    clip->share_guest_to_host = share_guest_to_host ? TRUE : FALSE;

    clip_log(clip, L"CLIP: Share host->guest %s, guest->host %s.",
             clip->share_host_to_guest ? L"ON" : L"OFF",
             clip->share_guest_to_host ? L"ON" : L"OFF");

    /* If host->guest was just switched back on while the window is focused,
       push the current host clipboard immediately so the toggle is instant. */
    if (clip->share_host_to_guest && clip->sync_enabled) {
        /* Sharing was off, so the guest may not have the current clipboard
           even if the sequence number did not change. Force a fresh push,
           queued so the UI thread never blocks on a file transfer. */
        InterlockedExchange(&clip->pushed_seq_valid, 0);
        clip_queue_push(clip);
    }
}

ASB_API void vm_clipboard_get_share_directions(VmClipboard clip,
                                               BOOL *share_host_to_guest,
                                               BOOL *share_guest_to_host)
{
    if (!clip) return;
    if (share_host_to_guest) *share_host_to_guest = clip->share_host_to_guest;
    if (share_guest_to_host) *share_guest_to_host = clip->share_guest_to_host;
}

ASB_API void vm_clipboard_on_clipboard_update(VmClipboard clip)
{
    if (!clip) return;

    /* Echo suppression */
    if (InterlockedExchange(&clip->writer_suppress, 0)) {
        clip_log(clip, L"CLIP: WM_CLIPBOARDUPDATE suppressed (echo).");
        return;
    }

    /* Focus/direction gate */
    if (!clip->sync_enabled || !clip->share_host_to_guest) return;

    clip_queue_push(clip);
}

ASB_API void vm_clipboard_on_reader_apply(VmClipboard clip)
{
    HGLOBAL handles[CLIP_MAX_FORMATS];
    int i, usable = 0;
    if (!clip) return;

    EnterCriticalSection(&clip->reader_cs);
    if (!clip->share_guest_to_host) {
        clip_reader_free_pending(clip);
        clip->reader_fmt_count = 0;
        LeaveCriticalSection(&clip->reader_cs);
        return;
    }
    /* Apply a copy once, and only when all of it has arrived: the message
       for one copy can be handled while the next copy is being fetched. */
    if (clip->reader_fmt_count == 0 || clip->reader_fetch_idx < clip->reader_fmt_count) {
        LeaveCriticalSection(&clip->reader_cs);
        return;
    }

    /* Build every handle before touching the clipboard, so a copy that
       brought nothing usable leaves the host clipboard as it was. Data gets
       two zero bytes past its end: text from the guest need not be
       terminated, and host apps read it as a C string. */
    for (i = 0; i < clip->reader_fmt_count; i++) {
        handles[i] = NULL;
        if (clip->reader_fmts[i].is_hdrop) {
            handles[i] = clip_build_hdrop_from_temp(clip->temp_dir);
        } else if (clip->reader_fmts[i].data && clip->reader_fmts[i].data_size > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                       (SIZE_T)clip->reader_fmts[i].data_size + sizeof(wchar_t));
            void *ptr = hMem ? GlobalLock(hMem) : NULL;
            if (ptr) {
                memcpy(ptr, clip->reader_fmts[i].data, clip->reader_fmts[i].data_size);
                GlobalUnlock(hMem);
                handles[i] = hMem;
            } else if (hMem) {
                GlobalFree(hMem);
            }
        }
        if (handles[i]) usable++;
    }

    if (usable > 0) {
        InterlockedExchange(&clip->writer_suppress, 1);
        if (OpenClipboard(clip->hwnd)) {
            EmptyClipboard();
            for (i = 0; i < clip->reader_fmt_count; i++) {
                /* On success the clipboard owns the handle. */
                if (handles[i] && SetClipboardData(clip->reader_fmts[i].local_id, handles[i]))
                    handles[i] = NULL;
            }

            /* The host clipboard now holds the guest's own content, and the
               guest already has it. Remember the new sequence number as
               "already pushed" so the next focus change does not push it
               straight back - that would make the guest delete and re-download
               its own file. Read it while the clipboard is still ours, so no
               other app can have changed it in between and made us record
               somebody else's content. */
            {
                DWORD seq = GetClipboardSequenceNumber();
                if (seq != 0) {
                    InterlockedExchange(&clip->pushed_seq, (LONG)seq);
                    InterlockedExchange(&clip->pushed_seq_valid, 1);
                    InterlockedExchange(&g_guest_content_seq, (LONG)seq);
                }
            }

            CloseClipboard();
            clip_log(clip, L"CLIP-R: Applied %d formats to host clipboard.", usable);
        }
        InterlockedExchange(&clip->writer_suppress, 0);
    }

    for (i = 0; i < clip->reader_fmt_count; i++)
        if (handles[i]) GlobalFree(handles[i]);

    clip_reader_free_pending(clip);
    clip->reader_fmt_count = 0;
    LeaveCriticalSection(&clip->reader_cs);
}
