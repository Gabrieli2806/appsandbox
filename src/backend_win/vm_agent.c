#include <winsock2.h>
#include "vm_agent.h"
#include "asb_events.h"
#include "vm_ssh_proxy.h"
#include "asb_core.h"
#include "hcn_network.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* Superseded by hcs_service_guid(vm->os_type, 1, ...) — kept for grep.
   Windows VMs end up reaching the byte-identical GUID via the helper. */
static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Agent status notification ---- */

#define WM_VM_AGENT_STATUS      (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN    (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY     (WM_APP + 4)
#define WM_VM_HYPERV_VIDEO_OFF  (WM_APP + 12)

static HWND g_agent_hwnd = NULL;

void vm_agent_set_hwnd(HWND hwnd)
{
    g_agent_hwnd = hwnd;
}

/* ---- Per-VM connection state ---- */

/* Longest line accepted from the agent, including the terminating NUL. The
   Windows agent's longest line is "log:" plus a 1023-character message, so
   every line it sends fits; longer lines are discarded (see recv_line). */
#define AGENT_LINE_MAX 2048

/* An over-long line is read and dropped up to its newline (see recv_line);
   past this many bytes without one the connection is dropped instead, so a
   guest that never sends a newline cannot hold the connection thread. */
#define AGENT_DISCARD_MAX (64 * 1024)

/* How long a tagged command waits for its reply before the connection is
   treated as dead and re-established. The agent handles commands one at a
   time and can be busy for a while: set_ip runs up to three netsh processes
   with a 10 s limit each, ssh_enable can wait for the SCM to start sshd
   (about 30 s), idd_connect restarts its helpers. Other lines (heartbeat,
   log:, gpu_copy_*, ...) are still read and handled while waiting, and a
   closed or failed connection is noticed at once. */
#define AGENT_REPLY_TIMEOUT_MS 60000

/* A fire-and-forget command (shutdown, restart) that could not be sent yet
   stays queued across a reconnect, but is dropped once it is this old: by
   then the shutdown watchdog has asked the user to force stop. */
#define AGENT_QUEUED_CMD_MAX_AGE_MS 120000

typedef struct AgentConn {
    /* Stable VM identifier; survives g_vms[] compaction. The actual
       VmInstance* is resolved via asb_find_vm_by_id() at each use. */
    UINT64         vm_id;
    HANDLE         thread;
    SOCKET         sock;
    volatile BOOL  stop;
    /* Command slot. One command at a time; a newer one replaces a queued one
       that has not been sent yet. Callers (vm_agent_send: idd_connect from
       the viewer's receive thread, shutdown/restart from the UI or headless
       API) and the connection thread use these fields only under the slot's
       lock (conn_lock), and waiters sleep on conn_cv. */
    BOOL           cmd_pending;  /* cmd is queued and not taken by the connection thread yet */
    LONG           cmd_gen;      /* generation of the newest queued command */
    BOOL           cmd_no_waiter;    /* queued fire-and-forget: kept across a reconnect */
    ULONGLONG      cmd_queued_at;    /* GetTickCount64() when it was queued */
    char           cmd[64];
    LONG           taken_gen;    /* newest generation the connection thread has taken */
    LONG           rsp_gen;      /* generation whose reply is in rsp (empty: failed) */
    LONG           done_gen;     /* newest generation that was answered, failed or dropped */
    char           rsp[AGENT_LINE_MAX];
    unsigned int   cmd_seq;      /* Monotonic sequence ID for tagged commands */
    /* Why the current connection dropped, for the host log. Not logged when
       it ended normally (os_shutdown, service_stopping, vm_agent_stop). */
    wchar_t        drop_reason[160];
    BOOL           drop_expected;    /* agent announced os_shutdown / service_stopping */
    BOOL           guest_shutting_down;  /* agent announced os_shutdown */
    BOOL           overlong_logged;  /* an over-long line was reported on this connection */
} AgentConn;

#define MAX_AGENTS 16
static AgentConn g_conns[MAX_AGENTS];
static BOOL      g_wsa_init = FALSE;

/* Per-slot command lock and wake-up, outside AgentConn so free_conn's reset
   never touches a lock or condition another thread may still be using.
   Initialised once and never deleted. */
static CRITICAL_SECTION   g_conn_cs[MAX_AGENTS];
static CONDITION_VARIABLE g_conn_cv[MAX_AGENTS];
static INIT_ONCE          g_conn_sync_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_conn_sync(PINIT_ONCE once, PVOID param, PVOID *context)
{
    int i;
    (void)once; (void)param; (void)context;
    for (i = 0; i < MAX_AGENTS; i++) {
        InitializeCriticalSection(&g_conn_cs[i]);
        InitializeConditionVariable(&g_conn_cv[i]);
    }
    return TRUE;
}

static CRITICAL_SECTION *conn_lock(AgentConn *conn)
{
    return &g_conn_cs[conn - g_conns];
}

static CONDITION_VARIABLE *conn_cv(AgentConn *conn)
{
    return &g_conn_cv[conn - g_conns];
}

static AgentConn *find_conn(VmInstance *vm)
{
    int i;
    if (!vm || vm->unique_id == 0) return NULL;
    for (i = 0; i < MAX_AGENTS; i++)
        if (g_conns[i].vm_id == vm->unique_id)
            return &g_conns[i];
    return NULL;
}

static AgentConn *alloc_conn(VmInstance *vm)
{
    int i;
    if (!vm || vm->unique_id == 0) return NULL;
    InitOnceExecuteOnce(&g_conn_sync_once, init_conn_sync, NULL, NULL);
    for (i = 0; i < MAX_AGENTS; i++) {
        if (g_conns[i].vm_id == 0) {
            EnterCriticalSection(&g_conn_cs[i]);
            memset(&g_conns[i], 0, sizeof(AgentConn));
            g_conns[i].vm_id = vm->unique_id;
            g_conns[i].sock = INVALID_SOCKET;
            LeaveCriticalSection(&g_conn_cs[i]);
            return &g_conns[i];
        }
    }
    return NULL;
}

static void free_conn(AgentConn *conn)
{
    if (conn->thread) CloseHandle(conn->thread);
    EnterCriticalSection(conn_lock(conn));
    memset(conn, 0, sizeof(AgentConn));
    conn->sock = INVALID_SOCKET;
    /* A caller still waiting in vm_agent_send sees no reply and returns. */
    WakeAllConditionVariable(conn_cv(conn));
    LeaveCriticalSection(conn_lock(conn));
}

/* ---- Line I/O ---- */

/* Read one line (up to \n) from the socket into buf, without the line end
   (\r is dropped). Returns 1 when a line was read (buf may be empty), 0 when
   the peer closed the socket, -1 on a socket error (WSAGetLastError() has
   the code). A line that does not fit in buf is read up to its \n and
   discarded: buf comes back empty and *discarded is set, so no part of it is
   handled as a line of its own. When AGENT_DISCARD_MAX bytes pass without a
   newline, -1 is returned with WSAEMSGSIZE. */
static int recv_line(SOCKET s, char *buf, int buf_size, BOOL *discarded)
{
    int pos = 0, dropped = 0;
    BOOL overflow = FALSE;

    if (discarded) *discarded = FALSE;
    for (;;) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) {
            buf[overflow ? 0 : pos] = '\0';
            return n;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (pos < buf_size - 1) {
            buf[pos++] = c;
        } else {
            overflow = TRUE;
            if (++dropped > AGENT_DISCARD_MAX) {
                buf[0] = '\0';
                WSASetLastError(WSAEMSGSIZE);
                return -1;
            }
        }
    }
    if (overflow) {
        pos = 0;
        if (discarded) *discarded = TRUE;
    }
    buf[pos] = '\0';
    return 1;
}

static int send_line(SOCKET s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    n = send(s, msg, len, 0);
    if (n <= 0) return n;
    n = send(s, "\n", 1, 0);
    return n;
}

/* Remember why the connection is about to drop; agent_thread_proc logs it
   after "Agent offline". */
static void set_drop_reason(AgentConn *conn, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(conn->drop_reason, ARRAYSIZE(conn->drop_reason), _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* recv_line on a connected agent socket: reports over-long lines once per
   connection and records why a failed read drops the connection. Same
   return values as recv_line. */
static int read_agent_line(AgentConn *conn, VmInstance *vm, SOCKET s,
                           char *buf, int buf_size)
{
    BOOL discarded;
    int n = recv_line(s, buf, buf_size, &discarded);

    if (n == 0) {
        set_drop_reason(conn, L"the agent closed the connection");
    } else if (n < 0 && WSAGetLastError() == WSAEMSGSIZE) {
        set_drop_reason(conn, L"the agent sent more than %d bytes without a line end",
                        AGENT_DISCARD_MAX);
    } else if (n < 0) {
        set_drop_reason(conn, L"receive error %d", WSAGetLastError());
    } else if (discarded && !conn->overlong_logged) {
        conn->overlong_logged = TRUE;
        ui_log(L"Agent for \"%s\" sent a line longer than %d bytes; "
               L"ignoring over-long lines on this connection.",
               vm->name, buf_size - 1);
    }
    return n;
}

/* Wait up to timeout_ms for data (or the peer's close) on the agent socket.
   Returns 1 when readable, 0 on timeout, -1 on a socket error (recorded as
   the drop reason). */
static int wait_readable(AgentConn *conn, SOCKET s, DWORD timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

    ret = select(0, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        set_drop_reason(conn, L"waiting for data failed (error %d)", WSAGetLastError());
        return -1;
    }
    return ret > 0 ? 1 : 0;
}

/* ---- RuntimeId lookup ---- */

static BOOL get_vm_runtime_id(VmInstance *instance, GUID *out)
{
    static const GUID zero_guid = {0};

    if (memcmp(&instance->runtime_id, &zero_guid, sizeof(GUID)) != 0) {
        *out = instance->runtime_id;
        return TRUE;
    }

    if (hcs_find_runtime_id(instance->name, out)) {
        instance->runtime_id = *out;
        return TRUE;
    }

    return FALSE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_agent(VmInstance *vm, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    GUID runtime_id;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;

    if (!get_vm_runtime_id(vm, &runtime_id))
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = runtime_id;
    hcs_service_guid(vm->os_type, 1, &addr.ServiceId);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with timeouts */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ---- Notify UI of agent status change ---- */

static void notify_agent_status(VmInstance *vm)
{
    asb_events_changed();
    if (g_agent_hwnd)
        PostMessageW(g_agent_hwnd, WM_VM_AGENT_STATUS, 0, (LPARAM)vm->unique_id);
}

/* Fire-and-forget: ask the guest agent to write the AppSandbox public key into
   authorized_keys. The guest replies async (untagged) "ssh_key_deployed" or
   "ssh_key_failed" (handled in process_async_message). Sent once SSH is ready;
   no-op if not requested, already done, or the key is missing. */
static void vm_agent_send_deploy_key(SOCKET s, VmInstance *vm)
{
    char cmd[640], pubkey_a[512];
    if (!vm->ssh_deploy_key || !vm->ssh_pubkey[0] || vm->ssh_key_deployed)
        return;
    WideCharToMultiByte(CP_UTF8, 0, vm->ssh_pubkey, -1, pubkey_a, sizeof(pubkey_a), NULL, NULL);
    sprintf_s(cmd, sizeof(cmd), "ssh_deploy_key %s", pubkey_a);
    send_line(s, cmd);
    ui_log(L"Requested SSH key deploy for \"%s\".", vm->name);
}

/* Process an untagged (async) message from the agent.
   Returns: 0 = handled, 1 = os_shutdown (caller should break),
            2 = service_stopping (caller should break and let the reconnect
                loop retry - SCM restarts the service on failure, so we must
                keep trying to reconnect). */
static int process_async_message(VmInstance *vm, SOCKET s, const char *buf)
{
    if (strcmp(buf, "heartbeat") == 0) {
        vm->last_heartbeat = GetTickCount64();
    } else if (strcmp(buf, "os_shutdown") == 0) {
        ui_log(L"Guest OS shutting down for \"%s\".", vm->name);
        vm->agent_online = FALSE;
        vm->idd_ready = FALSE;
        vm->shutdown_requested = TRUE;
        vm->shutdown_time = GetTickCount64();
        notify_agent_status(vm);
        return 1;
    } else if (strcmp(buf, "service_stopping") == 0) {
        ui_log(L"Agent service stopped in \"%s\".", vm->name);
        return 2;
    } else if (strncmp(buf, "gpu_copy_progress:", 18) == 0) {
        /* Every boot the agent syncs the host's GPU driver folders; files
           already there (same size) are skipped, so this is quick and only
           copies after a host driver update. Progress stays out of the log. */
    } else if (strncmp(buf, "gpu_copy_done:", 14) == 0) {
        if (atoi(buf + 14) > 0)
            ui_log(L"GPU drivers for \"%s\": %S files updated from this PC's driver.", vm->name, buf + 14);
        else
            ui_log(L"GPU drivers for \"%s\": up to date.", vm->name);
    } else if (strncmp(buf, "gpu_copy_error:", 15) == 0) {
        ui_log(L"GPU driver sync for \"%s\": %S", vm->name, buf + 15);
    } else if (strncmp(buf, "gpu_device_status:", 18) == 0) {
        ui_log(L"[%s] GPU: %S", vm->name, buf + 18);
    } else if (strcmp(buf, "gpu_device_ok") == 0) {
        ui_log(L"[%s] GPU device recovered successfully.", vm->name);
    } else if (strncmp(buf, "gpu_device_failed:", 18) == 0) {
        ui_log(L"[%s] GPU device still failing (problem %S).", vm->name, buf + 18);
    } else if (strncmp(buf, "idd_status:", 11) == 0) {
        /* Latch display readiness from the guest's own driver-state report
           ("running" via devcon). This is the non-destructive readiness
           signal -- it never touches the frame channel, so polling it can't
           steal the single consumer slot or blank the display. asb_vm_idd_ready
           gates display-open on it. */
        vm->idd_ready = (strcmp(buf + 11, "ok") == 0);
        ui_log(L"[%s] IDD driver: %S", vm->name, buf + 11);
        asb_events_changed();
    } else if (strncmp(buf, "hyperv_video:", 13) == 0) {
        ui_log(L"[%s] Hyper-V Video: %S", vm->name, buf + 13);
        /* NULL-guard like notify_agent_status: headless never sets the HWND,
           and PostMessageW(NULL, ...) would queue thread messages on this
           never-pumped agent thread. */
        vm->hyperv_video_off = strcmp(buf + 13, "disabled") == 0;
        asb_events_changed();
        if (g_agent_hwnd && vm->hyperv_video_off)
            PostMessageW(g_agent_hwnd, WM_VM_HYPERV_VIDEO_OFF, 0, (LPARAM)vm->unique_id);
    } else if (strncmp(buf, "displays:", 9) == 0) {
        ui_log(L"[%s] Displays: %S", vm->name, buf + 9);
    } else if (strncmp(buf, "log:", 4) == 0) {
        ui_log(L"[%s] %S", vm->name, buf + 4);
    } else if (strcmp(buf, "gpu_query") == 0) {
        if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
            char header[64];
            int gi;
            sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                      vm->gpu_shares.count);
            send_line(s, header);
            for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                char line[8192];
                char share_a[128], dest_a[512], filter_a[4096];
                WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                    share_a, sizeof(share_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                    dest_a, sizeof(dest_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                    filter_a, sizeof(filter_a), NULL, NULL);
                sprintf_s(line, sizeof(line), "%s|%s|%s", share_a, dest_a, filter_a);
                send_line(s, line);
            }
        } else {
            send_line(s, "gpu_none");
        }
    } else if (strcmp(buf, "ssh_ready") == 0) {
        vm->ssh_state = 2;
        vm_ssh_proxy_start(vm);
        ui_log(L"SSH ready for \"%s\".", vm->name);
        vm_agent_send_deploy_key(s, vm);   /* deploy the AppSandbox key now SSH is up */
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_key_deployed") == 0) {
        vm->ssh_key_deployed = TRUE;
        ui_log(L"SSH key deployed for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_key_failed") == 0) {
        ui_log(L"SSH key deploy FAILED for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_failed") == 0) {
        vm->ssh_state = 3;
        ui_log(L"SSH install failed for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_installing") == 0) {
        vm->ssh_state = 1;
        ui_log(L"SSH installing for \"%s\"...", vm->name);
        notify_agent_status(vm);
    }
    return 0;
}

/* Send a tagged command and wait up to AGENT_REPLY_TIMEOUT_MS for the tagged
   response. Processes any interleaved async messages while waiting.
   Returns: 1 on success (response, tag stripped, in rsp; may be empty),
   0 on close, -1 on error, reply timeout or stop request. On 0/-1 the
   connection must be dropped; conn->drop_reason says why. */
static int send_tagged_cmd(AgentConn *conn, SOCKET s, VmInstance *vm,
                           const char *cmd, char *rsp, int rsp_size)
{
    char tagged[512];
    char prefix[32];
    int pfx_len, n;
    ULONGLONG deadline;

    conn->cmd_seq++;
    sprintf_s(tagged, sizeof(tagged), "%u:%s", conn->cmd_seq, cmd);
    sprintf_s(prefix, sizeof(prefix), "%u:", conn->cmd_seq);
    pfx_len = (int)strlen(prefix);

    if (send_line(s, tagged) <= 0) {
        set_drop_reason(conn, L"sending %S failed (error %d)", cmd, WSAGetLastError());
        return -1;
    }

    deadline = GetTickCount64() + AGENT_REPLY_TIMEOUT_MS;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        int rc;

        /* vm_agent_stop also closes the socket; leave without a reason. */
        if (conn->stop) return -1;

        if (now >= deadline) {
            ui_log(L"Agent for \"%s\" did not answer %S within %d s; reconnecting.",
                   vm->name, cmd, AGENT_REPLY_TIMEOUT_MS / 1000);
            set_drop_reason(conn, L"no reply to %S within %d s",
                            cmd, AGENT_REPLY_TIMEOUT_MS / 1000);
            return -1;
        }

        /* Wait in short slices so a stop request is noticed promptly. */
        n = wait_readable(conn, s, (deadline - now < 200) ? (DWORD)(deadline - now) : 200);
        if (n < 0) return -1;
        if (n == 0) continue;

        n = read_agent_line(conn, vm, s, rsp, rsp_size);
        if (n <= 0) return n;

        if (strncmp(rsp, prefix, pfx_len) == 0) {
            /* Tagged response - strip prefix */
            memmove(rsp, rsp + pfx_len, strlen(rsp + pfx_len) + 1);
            return 1;
        }

        /* Untagged = async message, process inline. os_shutdown and
           service_stopping keep the wait going (the socket closes soon) but
           make that drop a normal one. */
        rc = process_async_message(vm, s, rsp);
        if (rc == 1 || rc == 2)
            conn->drop_expected = TRUE;
        if (rc == 1)
            conn->guest_shutting_down = TRUE;
    }
}

/* Record the outcome of the command of generation gen (reply text, or an
   empty reply when it failed) and wake the callers waiting for it. */
static void complete_cmd(AgentConn *conn, LONG gen, const char *reply)
{
    EnterCriticalSection(conn_lock(conn));
    strncpy_s(conn->rsp, sizeof(conn->rsp), reply, _TRUNCATE);
    conn->rsp_gen = gen;
    if (gen > conn->done_gen)
        conn->done_gen = gen;
    WakeAllConditionVariable(conn_cv(conn));
    LeaveCriticalSection(conn_lock(conn));
}

/* ---- Persistent connection thread ---- */

static DWORD WINAPI agent_thread_proc(LPVOID param)
{
    AgentConn *conn = (AgentConn *)param;
    VmInstance *vm;

    /* Resolve VM by stable ID each iteration. If asb_find_vm_by_id
       returns NULL the VM has been deleted (slot reclaimed) -- exit
       cleanly. Pointer freshness is now guaranteed for the body of
       each iteration; we never stash a stale &g_vms[idx]. */
    while (!conn->stop && (vm = asb_find_vm_by_id(conn->vm_id)) != NULL) {
        char buf[AGENT_LINE_MAX];
        int n;
        SOCKET s;

        /* Try to connect */
        s = connect_to_agent(vm, 3000);
        if (s == INVALID_SOCKET) {
            /* Retry in 3 seconds, checking stop flag each second */
            int wait;
            for (wait = 0; wait < 3000 && !conn->stop; wait += 500)
                Sleep(500);
            continue;
        }

        conn->sock = s;
        conn->drop_reason[0] = L'\0';
        conn->drop_expected = FALSE;
        conn->guest_shutting_down = FALSE;
        conn->overlong_logged = FALSE;

        /* Wait for hello from agent */
        n = recv_line(s, buf, sizeof(buf), NULL);
        if (n <= 0 || strcmp(buf, "hello") != 0) {
            closesocket(s);
            conn->sock = INVALID_SOCKET;
            continue;
        }

        vm->agent_online = TRUE;
        vm->idd_ready = FALSE;   /* re-evaluated by the agent's idd_status, sent right after hello */
        vm->shutdown_requested = FALSE;
        vm->last_heartbeat = GetTickCount64();
        ui_log(L"Agent online for \"%s\".", vm->name);

        /* Mark install complete on first agent connection */
        if (!vm->install_complete && !vm->is_template) {
            vm->install_complete = TRUE;
            vm_save_state_json(vm->vhdx_path, TRUE);
            ui_log(L"Install complete for \"%s\".", vm->name);
        }

        /* Send NAT IP to agent (only for NAT mode). Gateway is the chosen
           subnet's .1; prefix length is always /24 for our NAT. */
        if (vm->network_mode == NET_NAT && vm->nat_ip[0] != '\0') {
            char ip_cmd[64];
            sprintf_s(ip_cmd, sizeof(ip_cmd), "set_ip:%s/24:%s.1",
                       vm->nat_ip, hcn_nat_subnet_base());
            n = send_tagged_cmd(conn, s, vm, ip_cmd, buf, sizeof(buf));
            if (n <= 0) goto disconnected;
            ui_log(L"NAT IP config for \"%s\": %S", vm->name, buf);
        } else if (vm->network_mode == NET_EXTERNAL || vm->network_mode == NET_INTERNAL) {
            n = send_tagged_cmd(conn, s, vm, "set_dhcp", buf, sizeof(buf));
            if (n <= 0) goto disconnected;
            ui_log(L"DHCP network config for \"%s\": %S", vm->name, buf);
        }

        /* Send GPU share info to agent (if GPU-PV is assigned).
           Fire-and-forget - no response expected, so no tagging needed. */
        if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
            char header[64];
            int gi;
            sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                      vm->gpu_shares.count);
            send_line(s, header);
            for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                char line[8192];
                char share_a[128], dest_a[512], filter_a[4096];

                WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                    share_a, sizeof(share_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                    dest_a, sizeof(dest_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                    filter_a, sizeof(filter_a), NULL, NULL);

                sprintf_s(line, sizeof(line), "%s|%s|%s",
                          share_a, dest_a, filter_a);
                send_line(s, line);
            }
            ui_log(L"Sent %d GPU share(s) to agent for \"%s\".",
                   vm->gpu_shares.count, vm->name);
        } else {
            send_line(s, "gpu_none");
        }

        /* Request SSH install/enable if configured */
        if (vm->ssh_enabled) {
            n = send_tagged_cmd(conn, s, vm, "ssh_enable", buf, sizeof(buf));
            if (n <= 0) goto disconnected;
            if (strcmp(buf, "ssh_ready") == 0) {
                vm->ssh_state = 2;
                vm_ssh_proxy_start(vm);
                ui_log(L"SSH ready for \"%s\".", vm->name);
                vm_agent_send_deploy_key(s, vm);   /* deploy the AppSandbox key now SSH is up */
            } else if (strcmp(buf, "ssh_installing") == 0) {
                vm->ssh_state = 1;
                ui_log(L"SSH installing for \"%s\"...", vm->name);
            } else if (strcmp(buf, "ssh_failed") == 0) {
                vm->ssh_state = 3;
                ui_log(L"SSH install failed for \"%s\".", vm->name);
            }
        }

        /* Notify UI - agent online + SSH state are all set now */
        notify_agent_status(vm);

        /* Connected - read loop */
        while (!conn->stop) {
            int ret;
            BOOL have_cmd = FALSE, expired = FALSE;
            char cmd[sizeof(conn->cmd)];
            LONG gen = 0;

            /* Take the queued command, if any. A command queued while this
               one is in flight stays pending and is sent on a later pass. */
            EnterCriticalSection(conn_lock(conn));
            if (conn->cmd_pending) {
                conn->cmd_pending = FALSE;
                gen = conn->cmd_gen;
                conn->taken_gen = gen;
                memcpy(cmd, conn->cmd, sizeof(cmd));
                cmd[sizeof(cmd) - 1] = '\0';
                expired = conn->cmd_no_waiter &&
                          GetTickCount64() - conn->cmd_queued_at > AGENT_QUEUED_CMD_MAX_AGE_MS;
                have_cmd = TRUE;
            }
            LeaveCriticalSection(conn_lock(conn));

            if (have_cmd && expired) {
                ui_log(L"Agent for \"%s\": \"%S\" was queued too long ago and is not sent.",
                       vm->name, cmd);
                complete_cmd(conn, gen, "");
                continue;
            }
            if (have_cmd) {
                char reply[AGENT_LINE_MAX];

                /* Send it and read lines until we get our tagged response */
                n = send_tagged_cmd(conn, s, vm, cmd, reply, sizeof(reply));
                complete_cmd(conn, gen, n > 0 ? reply : "");
                if (n <= 0)
                    goto disconnected;
                continue;
            }

            /* Wait for data with 200ms timeout */
            ret = wait_readable(conn, s, 200);
            if (ret < 0) break;
            if (ret == 0) continue; /* timeout - loop back to check cmd_pending/stop */

            n = read_agent_line(conn, vm, s, buf, sizeof(buf));
            if (n <= 0) break; /* connection lost */

            { int rc = process_async_message(vm, s, buf);
              if (rc == 1 || rc == 2) {   /* os_shutdown or service_stopping - reconnect loop will retry */
                  conn->drop_expected = TRUE;
                  conn->guest_shutting_down = rc == 1;
                  break;
              }
            }
        }

        disconnected:
        /* Connection lost */
        vm->agent_online = FALSE;
        vm->idd_ready = FALSE;
        /* Atomically claim the socket so we never double-close a handle that
           vm_agent_stop() may have already closed (and whose value could have
           been recycled by another socket()/accept()). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&conn->sock, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET)
                closesocket(old);
        }
        ui_log(L"Agent offline for \"%s\".", vm->name);
        /* Say why, unless the agent announced it (os_shutdown,
           service_stopping) or the host stopped the connection. */
        if (!conn->stop && !conn->drop_expected && conn->drop_reason[0])
            ui_log(L"Agent connection for \"%s\" dropped: %s.", vm->name, conn->drop_reason);
        notify_agent_status(vm);

        /* A queued command that was not sent yet: fail it when a caller
           waits for its reply. A fire-and-forget one (shutdown, restart)
           stays queued for the next connection, unless Windows in the VM is
           already shutting down. */
        {
            BOOL failed = FALSE, kept = FALSE;
            char queued[sizeof(conn->cmd)];
            LONG queued_gen = 0;

            EnterCriticalSection(conn_lock(conn));
            if (conn->cmd_pending) {
                memcpy(queued, conn->cmd, sizeof(queued));
                queued[sizeof(queued) - 1] = '\0';
                queued_gen = conn->cmd_gen;
                if (conn->cmd_no_waiter && !conn->guest_shutting_down && !conn->stop &&
                    vm->running) {
                    kept = TRUE;
                } else {
                    conn->cmd_pending = FALSE;
                    failed = TRUE;
                }
            }
            LeaveCriticalSection(conn_lock(conn));

            if (failed) {
                complete_cmd(conn, queued_gen, "");
                ui_log(L"Agent for \"%s\": \"%S\" was not sent (connection dropped).",
                       vm->name, queued);
            } else if (kept) {
                ui_log(L"Agent for \"%s\": \"%S\" is sent when the agent reconnects.",
                       vm->name, queued);
            }
        }

        /* Don't reconnect if the VM is no longer running */
        if (!vm->running)
            break;
    }

    return 0;
}

/* ---- Public API ---- */

void vm_agent_start(VmInstance *instance)
{
    /* Manual-install VMs have no App Sandbox guest agent. */
    if (instance && _wcsicmp(instance->os_type, L"Other") == 0) return;
    AgentConn *conn;
    WSADATA wsa;

    if (!g_wsa_init) {
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsa_init = TRUE;
    }

    /* Already running? */
    conn = find_conn(instance);
    if (conn && conn->thread) return;

    conn = alloc_conn(instance);
    if (!conn) {
        ui_log(L"Agent: too many connections");
        return;
    }

    conn->stop = FALSE;
    conn->thread = CreateThread(NULL, 0, agent_thread_proc, conn, 0, NULL);
}

void vm_agent_stop(VmInstance *instance)
{
    AgentConn *conn = find_conn(instance);
    if (!conn) return;

    conn->stop = TRUE;

    /* Unblock recv/select by closing the socket. Atomically claim it so we
       never double-close a handle the agent thread may close concurrently at
       its disconnected: label (a recycled value could close a live unrelated
       socket). */
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&conn->sock, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET)
            closesocket(old);
    }

    if (conn->thread) {
        WaitForSingleObject(conn->thread, 5000);
    }

    instance->agent_online = FALSE;
    instance->idd_ready = FALSE;
    free_conn(conn);
    notify_agent_status(instance);
}

BOOL vm_agent_send(VmInstance *instance, const char *command,
                   char *response, int response_max, DWORD timeout_ms)
{
    AgentConn *conn = find_conn(instance);
    CRITICAL_SECTION *lock;
    char reply[AGENT_LINE_MAX];
    ULONGLONG deadline;
    LONG my_gen;
    BOOL answered = FALSE, superseded = FALSE, ok;

    if (!conn || !instance->agent_online) {
        ui_log(L"Agent: not connected to \"%s\"", instance->name);
        return FALSE;
    }
    if (strlen(command) >= sizeof(conn->cmd)) {
        ui_log(L"Agent: command \"%S\" is too long", command);
        return FALSE;
    }
    lock = conn_lock(conn);

    /* Hand the command to the connection thread (it owns the socket and is the
       sole sender). The slot holds one command: a newer one replaces a queued
       one that was not sent yet (its caller then gets FALSE). A caller that
       gives up waiting does not stop the connection thread waiting for that
       reply (up to AGENT_REPLY_TIMEOUT_MS); a command queued meanwhile stays
       pending and is sent once that reply is in. */
    EnterCriticalSection(lock);
    if (conn->cmd_pending)   /* replaced: its caller, if waiting, gives up */
        WakeAllConditionVariable(conn_cv(conn));
    my_gen = ++conn->cmd_gen;
    strcpy_s(conn->cmd, sizeof(conn->cmd), command);
    conn->cmd_no_waiter = timeout_ms == 0;
    conn->cmd_queued_at = GetTickCount64();
    conn->cmd_pending = TRUE;

    /* timeout_ms == 0  =>  FIRE-AND-FORGET. Used for shutdown/restart, which ride
       the guest powering off: the agent replies "ok" then kills itself, so there
       is no reliable synchronous reply to wait for. The connection thread sends
       the queued command and consumes the (ignored) reply on its OWN read loop;
       we return immediately, so we never block the single-threaded HTTP request
       loop / the GUI thread. Delivery is confirmed by the HCS SystemExited
       monitor, not by this reply. If the connection drops before it is sent,
       it is sent on the next connection (see agent_thread_proc). */
    if (timeout_ms == 0) {
        LeaveCriticalSection(lock);
        return TRUE;
    }

    /* Otherwise wait up to timeout_ms for the agent's tagged reply (idd_connect
       expects a prompt "ok"). A real disconnect unblocks us: agent_thread_proc
       completes the command with an empty reply when the connection drops
       (closed, socket error, or no reply within AGENT_REPLY_TIMEOUT_MS), so we
       return FALSE. */
    deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        ULONGLONG now;
        if (conn->rsp_gen == my_gen) {
            strncpy_s(reply, sizeof(reply), conn->rsp, _TRUNCATE);
            answered = TRUE;
            break;
        }
        if (conn->done_gen >= my_gen || conn->vm_id == 0 ||
            (conn->cmd_gen != my_gen && conn->taken_gen < my_gen)) {
            superseded = TRUE;   /* replaced before it was sent, or the connection was freed */
            break;
        }
        now = GetTickCount64();
        if (now >= deadline)
            break;
        SleepConditionVariableCS(conn_cv(conn), lock, (DWORD)(deadline - now));
    }
    /* Timed out: withdraw the command if it is still queued and still ours. */
    if (!answered && !superseded && conn->cmd_pending && conn->cmd_gen == my_gen)
        conn->cmd_pending = FALSE;
    LeaveCriticalSection(lock);

    if (superseded) {
        ui_log(L"Agent: command \"%S\" was not answered (replaced by a newer command "
               L"or the connection closed)", command);
        return FALSE;
    }
    if (!answered) {
        ui_log(L"Agent: command \"%S\" timed out", command);
        return FALSE;
    }

    if (response && response_max > 0)
        strncpy_s(response, response_max, reply, _TRUNCATE);

    if (!reply[0]) {
        ui_log(L"Agent: command \"%S\" failed (connection dropped)", command);
        return FALSE;
    }
    ok = (strcmp(reply, "ok") == 0);
    ui_log(L"Agent: %S -> %S", command, reply);
    return ok;
}

BOOL vm_agent_shutdown(VmInstance *instance)
{
    return vm_agent_send(instance, "shutdown", NULL, 0, 0);   /* 0 = fire-and-forget */
}

BOOL vm_agent_restart(VmInstance *instance)
{
    return vm_agent_send(instance, "restart", NULL, 0, 0);   /* 0 = fire-and-forget */
}

BOOL vm_agent_ping(VmInstance *instance)
{
    return vm_agent_send(instance, "ping", NULL, 0, 5000);
}
