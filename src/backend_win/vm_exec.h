#ifndef VM_EXEC_H
#define VM_EXEC_H

#include <windows.h>

/* DLL export/import */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/*
 * Host side of the guest command channel (src/core/asb_exec_protocol.h):
 * run a command in a guest, write a file into it, read a file out of it.
 * Each call opens its own connection to the guest agent, so calls for the
 * same VM can run in parallel and none of them uses the agent's control
 * connection. Blocking; call from a worker thread. Thread-safe.
 */

/* How a call ended. Anything but ASB_GUEST_OK leaves `message` with a
   sentence for the user. */
#define ASB_GUEST_OK            0
#define ASB_GUEST_NO_VM         1  /* no such VM, or it is not running */
#define ASB_GUEST_NO_CHANNEL    2  /* nothing listens: guest still booting, or its agent is older than this channel */
#define ASB_GUEST_IO            3  /* the connection broke (guest restarted, crashed or was stopped) */
#define ASB_GUEST_REFUSED       4  /* the guest answered with an error (message has its text) */
#define ASB_GUEST_TOO_LARGE     5  /* read: the file is larger than the limit (total = its size) */
#define ASB_GUEST_HOST_FILE     6  /* reading or writing the host-side file failed */

typedef struct {
    int      result;        /* ASB_GUEST_* */
    DWORD    os_error;      /* guest error code (ASB_GUEST_REFUSED) or host one (ASB_GUEST_HOST_FILE) */
    DWORD    exit_code;     /* exec */
    BOOL     timed_out;     /* exec: ended by the time limit */
    BOOL     out_truncated, err_truncated;
    char    *out;           /* exec: stdout, UTF-8, NUL-terminated; free with asb_guest_free */
    DWORD    out_len;
    char    *err;           /* exec: stderr */
    DWORD    err_len;
    UINT64   total;         /* read: bytes copied, or the file's size when too large */
    char     message[640];  /* UTF-8 */
} AsbGuestResult;

/* Which guest to talk to: a copy of what the calls need from the VM's
   record, so they never touch the core's VM table from their own thread.
   Fill it with asb_guest_target where the core is used (the window thread,
   or the request context of the local API). */
typedef struct {
    BOOL    found, running, installing, agent_online, has_agent;
    BOOL    has_exec;   /* the agent announced the command channel ("caps:exec") */
    GUID    runtime_id;
    wchar_t os_type[32];
    wchar_t name[256];
} AsbGuestTarget;

ASB_API void asb_guest_target(UINT64 vm_id, AsbGuestTarget *target);

/* Run command in the guest as SYSTEM (Windows) or root (Linux). flags:
   ASB_EXEC_SHELL runs it through cmd.exe /c or /bin/sh -c; without it the
   command line is started as it is (Windows guests; Linux guests always use
   the shell). ASB_EXEC_DESKTOP (Windows guests whose agent announces
   "desktop" in asb_guest_ping) starts it in the signed-in user's desktop
   session. cwd may be NULL. max_output limits stdout and stderr separately. */
#define ASB_EXEC_SHELL    0x1u
#define ASB_EXEC_DESKTOP  0x2u
#define ASB_EXEC_AS_USER  0x4u   /* as the signed-in user (agents that announce "user") */
#define ASB_EXEC_ELEVATED 0x8u   /* with AS_USER: their administrator token when they have one */
ASB_API int asb_guest_exec(const AsbGuestTarget *vm, const char *command_utf8, const char *cwd_utf8,
                           DWORD flags, DWORD timeout_ms, DWORD max_output, AsbGuestResult *r);

/* Write a file in the guest (parent folders are created, an existing file is
   replaced). The content is `size` bytes from `file` (an open host file,
   read from its current position) or, when file is NULL, from `data`. */
ASB_API int asb_guest_write_file(const AsbGuestTarget *vm, const char *guest_path_utf8,
                                 HANDLE file, const void *data, UINT64 size, AsbGuestResult *r);

/* Read a guest file of at most `limit` bytes into the open host file `file`,
   or, when file is NULL, into memory: r->out / r->out_len (at most 64 MB). */
ASB_API int asb_guest_read_file(const AsbGuestTarget *vm, const char *guest_path_utf8,
                                HANDLE file, UINT64 limit, AsbGuestResult *r);

/* "windows" or "linux" (and the agent's extras) in r->message when the
   guest's command channel answers. One attempt, no waiting: use it to probe. */
ASB_API int asb_guest_ping(const AsbGuestTarget *vm, AsbGuestResult *r);

/* Send `count` 20-byte input packets (src/core/protocol.h) to the guest's
   input helper on its own connection, gap_ms apart. Fails while the VM's
   window is open: the helper serves one connection, and the window has it. */
ASB_API int asb_guest_send_input(const AsbGuestTarget *vm, const void *packets, int count,
                                 DWORD gap_ms, AsbGuestResult *r);

ASB_API void asb_guest_free(AsbGuestResult *r);

#endif /* VM_EXEC_H */
