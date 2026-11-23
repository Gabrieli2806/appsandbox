#include <winsock2.h>
#include "vm_exec.h"
#include "asb_core.h"
#include "hcs_vm.h"
#include "../core/asb_exec_protocol.h"
#include <stdio.h>
#include <stdlib.h>

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

#define EXEC_CONNECT_TIMEOUT_MS  4000
#define EXEC_IO_TIMEOUT_MS       60000
/* The guest ends the command at its time limit and then answers; this much
   longer and the connection is treated as dead. */
#define EXEC_REPLY_MARGIN_MS     20000
#define EXEC_FILE_CHUNK          (256 * 1024)
#define EXEC_MAX_TEXT            (64u * 1024)   /* longest message accepted from the guest */

static void set_message(AsbGuestResult *r, int result, const char *text)
{
    r->result = result;
    strncpy_s(r->message, sizeof(r->message), text, _TRUNCATE);
}

static BOOL send_all(SOCKET s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n; len -= (size_t)n;
    }
    return TRUE;
}

static BOOL recv_all(SOCKET s, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) return FALSE;
        p += n; len -= (size_t)n;
    }
    return TRUE;
}

ASB_API void asb_guest_target(UINT64 vm_id, AsbGuestTarget *target)
{
    VmInstance *vm = asb_find_vm_by_id(vm_id);
    memset(target, 0, sizeof(*target));
    if (!vm) return;
    target->found = TRUE;
    target->running = vm->running;
    target->installing = !vm->install_complete;
    target->agent_online = vm->agent_online;
    target->has_agent = _wcsicmp(vm->os_type, L"Other") != 0;
    target->has_exec = (vm->agent_caps & VM_AGENT_CAP_EXEC) != 0;
    target->runtime_id = vm->runtime_id;
    wcsncpy_s(target->os_type, ARRAYSIZE(target->os_type), vm->os_type, _TRUNCATE);
    wcsncpy_s(target->name, ARRAYSIZE(target->name), vm->name, _TRUNCATE);
}

/* Connect to the VM's command channel. INVALID_SOCKET with r filled in when
   the VM is not running or nothing listens. */
static SOCKET channel_connect(const AsbGuestTarget *vm, unsigned channel, AsbGuestResult *r);

/* How long a call keeps trying when the agent is known to have the channel
   but does not accept yet (its listener starts a little after the agent). */
#define EXEC_CONNECT_RETRY_MS  12000

static SOCKET exec_connect_wait(const AsbGuestTarget *vm, AsbGuestResult *r, DWORD retry_ms)
{
    ULONGLONG deadline = GetTickCount64() + retry_ms;
    for (;;) {
        SOCKET s = channel_connect(vm, ASB_EXEC_CHANNEL, r);
        if (s != INVALID_SOCKET || r->result != ASB_GUEST_NO_CHANNEL || !vm->has_exec || !vm->agent_online ||
            GetTickCount64() >= deadline)
            return s;
        Sleep(1000);
        r->result = ASB_GUEST_OK;
    }
}

static SOCKET exec_connect(const AsbGuestTarget *vm, AsbGuestResult *r)
{
    return exec_connect_wait(vm, r, EXEC_CONNECT_RETRY_MS);
}

static SOCKET channel_connect(const AsbGuestTarget *vm, unsigned channel, AsbGuestResult *r)
{
    static const GUID zero_guid = { 0 };
    static volatile LONG wsa_started;
    SOCKADDR_HV addr;
    SOCKET s;
    u_long nonblock = 1;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD timeout = EXEC_IO_TIMEOUT_MS;

    if (!InterlockedExchange(&wsa_started, 1)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    if (!vm || !vm->found || !vm->running) {
        set_message(r, ASB_GUEST_NO_VM, "The VM is not running. Start it with start_vm and wait with wait_for_vm.");
        return INVALID_SOCKET;
    }
    if (!vm->has_agent) {
        set_message(r, ASB_GUEST_NO_CHANNEL,
                    "This is a manual-install VM (OS type Other): it has no App Sandbox guest agent, "
                    "so commands and file copies are not available.");
        return INVALID_SOCKET;
    }
    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = vm->runtime_id;
    if (memcmp(&addr.VmId, &zero_guid, sizeof(GUID)) == 0 && !hcs_find_runtime_id(vm->name, &addr.VmId)) {
        set_message(r, ASB_GUEST_NO_VM, "The VM is still starting. Call wait_for_vm and try again.");
        return INVALID_SOCKET;
    }
    hcs_service_guid(vm->os_type, channel, &addr.ServiceId);

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s != INVALID_SOCKET) {
        ioctlsocket(s, FIONBIO, &nonblock);
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            BOOL ok = FALSE;
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                FD_ZERO(&wfds); FD_ZERO(&efds);
                FD_SET(s, &wfds); FD_SET(s, &efds);
                tv.tv_sec = EXEC_CONNECT_TIMEOUT_MS / 1000;
                tv.tv_usec = (EXEC_CONNECT_TIMEOUT_MS % 1000) * 1000;
                ok = select(0, NULL, &wfds, &efds, &tv) > 0 && !FD_ISSET(s, &efds);
            }
            if (!ok) { closesocket(s); s = INVALID_SOCKET; }
        }
    }
    if (s == INVALID_SOCKET && channel != ASB_EXEC_CHANNEL) {
        set_message(r, ASB_GUEST_NO_CHANNEL, "The guest does not accept that connection right now.");
        return INVALID_SOCKET;
    }
    if (s == INVALID_SOCKET) {
        if (vm->installing)
            set_message(r, ASB_GUEST_NO_CHANNEL,
                        "The operating system is still being installed in this VM. Call wait_for_vm until it is online.");
        else if (!vm->agent_online)
            set_message(r, ASB_GUEST_NO_CHANNEL,
                        "The guest agent is not online (the VM is booting, restarting or has crashed). "
                        "Call wait_for_vm, then try again; get_log shows what happened.");
        else if (vm->has_exec)
            set_message(r, ASB_GUEST_NO_CHANNEL,
                        "The guest agent is online but its command channel does not accept connections yet "
                        "(it starts a little after the agent, later after a restart of the guest). Call "
                        "wait_for_vm, which waits for it, then try again.");
        else
            set_message(r, ASB_GUEST_NO_CHANNEL,
                        "The guest agent in this VM is too old for commands and file copies. Update its guest "
                        "components: shut the VM down (shutdown_vm), then start it again - with \"Update guest "
                        "components automatically\" on (Settings, the default) App Sandbox updates a Windows VM "
                        "before it starts; otherwise use Update on the VM's page. Linux VMs created before this "
                        "version need the appsandbox-agent rebuilt from the current sources.");
        return INVALID_SOCKET;
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    return s;
}

static BOOL send_request(SOCKET s, UINT32 op, UINT32 flags, UINT32 timeout_ms, UINT32 limit,
                         const char *a, const char *b, UINT64 data_len)
{
    AsxRequest q;
    memset(&q, 0, sizeof(q));
    q.magic = ASX_REQUEST_MAGIC; q.version = ASX_VERSION;
    q.op = op; q.flags = flags; q.timeout_ms = timeout_ms; q.limit = limit;
    q.a_len = a ? (UINT32)strlen(a) : 0;
    q.b_len = b ? (UINT32)strlen(b) : 0;
    q.data_len = data_len;
    return send_all(s, &q, sizeof(q)) && (!q.a_len || send_all(s, a, q.a_len)) &&
           (!q.b_len || send_all(s, b, q.b_len));
}

/* Read the response header and its text. FALSE (r filled in) when the
   connection broke or the guest refused. */
static BOOL read_response(SOCKET s, AsxResponse *p, AsbGuestResult *r)
{
    char text[sizeof(r->message)];
    UINT32 keep;

    /* The guest is not trusted: every length it sends is checked before use. */
    if (!recv_all(s, p, sizeof(*p)) || p->magic != ASX_RESPONSE_MAGIC || p->text_len > EXEC_MAX_TEXT) {
        set_message(r, ASB_GUEST_IO,
                    "The connection to the guest agent broke before it answered: the VM restarted, crashed "
                    "or was stopped. Call wait_for_vm, then get_guest_crash or get_log to see what happened.");
        return FALSE;
    }
    keep = p->text_len < sizeof(text) - 1 ? p->text_len : (UINT32)sizeof(text) - 1;
    if (keep && !recv_all(s, text, keep)) {
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while it answered.");
        return FALSE;
    }
    text[keep] = '\0';
    /* Skip the part of a longer text that was not kept. */
    for (keep = p->text_len - keep; keep > 0; ) {
        char skip[256];
        UINT32 n = keep > sizeof(skip) ? (UINT32)sizeof(skip) : keep;
        if (!recv_all(s, skip, n)) break;
        keep -= n;
    }
    if (p->status == ASX_STATUS_OK) {
        strncpy_s(r->message, sizeof(r->message), text, _TRUNCATE);
        return TRUE;
    }
    r->os_error = p->os_error;
    r->total = p->total;
    if (p->status == ASX_STATUS_TOO_LARGE)
        set_message(r, ASB_GUEST_TOO_LARGE, "The file is larger than the limit.");
    else
        set_message(r, ASB_GUEST_REFUSED, text[0] ? text : "The guest agent refused the request.");
    return FALSE;
}

ASB_API int asb_guest_ping(const AsbGuestTarget *vm, AsbGuestResult *r)
{
    AsxResponse p;
    SOCKET s;
    memset(r, 0, sizeof(*r));
    s = exec_connect_wait(vm, r, 0);
    if (s == INVALID_SOCKET) return r->result;
    if (!send_request(s, ASX_OP_PING, 0, 0, 0, NULL, NULL, 0))
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke.");
    else
        read_response(s, &p, r);
    closesocket(s);
    return r->result;
}

ASB_API int asb_guest_exec(const AsbGuestTarget *vm, const char *command_utf8, const char *cwd_utf8,
                           DWORD flags, DWORD timeout_ms, DWORD max_output, AsbGuestResult *r)
{
    AsxResponse p;
    SOCKET s;
    DWORD wait;
    /* The guest converts console output to UTF-8 after cutting it at the
       limit, which can make it up to three times longer. */
    DWORD accept;

    memset(r, 0, sizeof(*r));
    if (!command_utf8 || !command_utf8[0] || strlen(command_utf8) > ASX_MAX_STRING ||
        (cwd_utf8 && strlen(cwd_utf8) > ASX_MAX_STRING)) {
        set_message(r, ASB_GUEST_REFUSED, "The command is empty or longer than 32768 bytes.");
        return r->result;
    }
    if (max_output > ASX_MAX_OUTPUT) max_output = ASX_MAX_OUTPUT;
    if (max_output == 0) max_output = 65536;
    accept = max_output * 3 + 16;
    s = exec_connect(vm, r);
    if (s == INVALID_SOCKET) return r->result;

    wait = timeout_ms + EXEC_REPLY_MARGIN_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&wait, sizeof(wait));
    if (!send_request(s, ASX_OP_EXEC, flags & (ASX_EXEC_SHELL | ASX_EXEC_DESKTOP | ASX_EXEC_AS_USER | ASX_EXEC_ELEVATED),
                      timeout_ms, max_output,
                      command_utf8, cwd_utf8, 0)) {
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while sending the command.");
    } else if (read_response(s, &p, r)) {
        if (p.out_len > accept || p.err_len > accept) {   /* more than was asked for */
            set_message(r, ASB_GUEST_IO, "The guest agent sent an answer that is not valid.");
        } else {
            r->out = (char *)malloc((size_t)p.out_len + 1);
            r->err = (char *)malloc((size_t)p.err_len + 1);
            if (!r->out || !r->err || (p.out_len && !recv_all(s, r->out, p.out_len)) ||
                (p.err_len && !recv_all(s, r->err, p.err_len))) {
                asb_guest_free(r);
                set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while it sent the output.");
            } else {
                r->out[p.out_len] = '\0'; r->err[p.err_len] = '\0';
                r->out_len = p.out_len; r->err_len = p.err_len;
                r->exit_code = p.exit_code;
                r->timed_out = (p.flags & ASX_FLAG_TIMED_OUT) != 0;
                r->out_truncated = (p.flags & ASX_FLAG_OUT_TRUNCATED) != 0;
                r->err_truncated = (p.flags & ASX_FLAG_ERR_TRUNCATED) != 0;
                r->message[0] = '\0';
            }
        }
    }
    closesocket(s);
    return r->result;
}

ASB_API int asb_guest_write_file(const AsbGuestTarget *vm, const char *guest_path_utf8,
                                 HANDLE file, const void *data, UINT64 size, AsbGuestResult *r)
{
    AsxResponse p;
    SOCKET s;
    char *chunk = NULL;
    UINT64 left = size;

    memset(r, 0, sizeof(*r));
    if (!guest_path_utf8 || !guest_path_utf8[0] || strlen(guest_path_utf8) > ASX_MAX_STRING) {
        set_message(r, ASB_GUEST_REFUSED, "The guest path is empty or too long.");
        return r->result;
    }
    s = exec_connect(vm, r);
    if (s == INVALID_SOCKET) return r->result;
    if (!send_request(s, ASX_OP_WRITE_FILE, 0, 0, 0, guest_path_utf8, NULL, size)) {
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke.");
        goto done;
    }
    if (file) {
        chunk = (char *)malloc(EXEC_FILE_CHUNK);
        while (chunk && left > 0) {
            DWORD n = left > EXEC_FILE_CHUNK ? EXEC_FILE_CHUNK : (DWORD)left, got = 0;
            if (!ReadFile(file, chunk, n, &got, NULL) || got == 0) {
                r->os_error = GetLastError();
                set_message(r, ASB_GUEST_HOST_FILE, "Reading the file on this PC failed part-way; nothing was written in the VM.");
                goto done;   /* closing the socket makes the guest drop its partial file */
            }
            if (!send_all(s, chunk, got)) break;
            left -= got;
        }
    } else if (size && send_all(s, data, (size_t)size)) {
        left = 0;
    }
    if (left > 0) {
        /* The guest may have refused (bad path, disk full) and closed: its
           answer says why. */
        if (!read_response(s, &p, r) && r->result == ASB_GUEST_IO)
            set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while the file was sent.");
        else if (r->result == ASB_GUEST_OK)
            set_message(r, ASB_GUEST_IO, "The guest agent stopped reading the file.");
        goto done;
    }
    if (read_response(s, &p, r)) {
        r->total = size;
        r->message[0] = '\0';
    }
done:
    free(chunk);
    closesocket(s);
    return r->result;
}

ASB_API int asb_guest_read_file(const AsbGuestTarget *vm, const char *guest_path_utf8,
                                HANDLE file, UINT64 limit, AsbGuestResult *r)
{
    AsxResponse p;
    SOCKET s;
    char *chunk = NULL;
    UINT64 left;

    memset(r, 0, sizeof(*r));
    if (!guest_path_utf8 || !guest_path_utf8[0] || strlen(guest_path_utf8) > ASX_MAX_STRING) {
        set_message(r, ASB_GUEST_REFUSED, "The guest path is empty or too long.");
        return r->result;
    }
    if (!file && limit > 64u * 1024 * 1024) limit = 64u * 1024 * 1024;
    s = exec_connect(vm, r);
    if (s == INVALID_SOCKET) return r->result;
    if (!send_request(s, ASX_OP_READ_FILE, 0, 0, 0, guest_path_utf8, NULL, limit)) {
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke.");
        goto done;
    }
    if (!read_response(s, &p, r)) goto done;
    if (p.total > limit) {
        set_message(r, ASB_GUEST_IO, "The guest agent sent an answer that is not valid.");
        goto done;
    }
    if (!file) {   /* into memory (r->out) */
        r->out = (char *)malloc((size_t)p.total + 1);
        if (!r->out || (p.total && !recv_all(s, r->out, (size_t)p.total))) {
            asb_guest_free(r);
            set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while the file was received.");
            goto done;
        }
        r->out[p.total] = '\0';
        r->out_len = (DWORD)p.total;
        r->total = p.total;
        r->message[0] = '\0';
        goto done;
    }
    chunk = (char *)malloc(EXEC_FILE_CHUNK);
    for (left = p.total; chunk && left > 0; ) {
        DWORD n = left > EXEC_FILE_CHUNK ? EXEC_FILE_CHUNK : (DWORD)left, wrote = 0;
        if (!recv_all(s, chunk, n)) break;
        if (!WriteFile(file, chunk, n, &wrote, NULL) || wrote != n) {
            r->os_error = GetLastError();
            set_message(r, ASB_GUEST_HOST_FILE, "Writing the file on this PC failed (disk full?).");
            goto done;
        }
        left -= n;
    }
    if (left > 0) {
        set_message(r, ASB_GUEST_IO, "The connection to the guest agent broke while the file was received.");
        goto done;
    }
    r->total = p.total;
    r->message[0] = '\0';
done:
    free(chunk);
    closesocket(s);
    return r->result;
}

/* Input packets go to the guest's input helper (channel 3), which serves one
   connection at a time: the VM's window, when it is open, has it. */
ASB_API int asb_guest_send_input(const AsbGuestTarget *vm, const void *packets, int count,
                                 DWORD gap_ms, AsbGuestResult *r)
{
    const char *p = (const char *)packets;
    UINT32 ready = 0;
    DWORD wait = 6000;
    SOCKET s;
    int i;

    memset(r, 0, sizeof(*r));
    s = channel_connect(vm, 3, r);
    if (s == INVALID_SOCKET) {
        if (r->result == ASB_GUEST_NO_CHANNEL)
            set_message(r, ASB_GUEST_NO_CHANNEL,
                        "The guest's input helper is not reachable: nobody is signed in yet, the VM is still "
                        "booting, or the VM's window on this PC is using it.");
        return r->result;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&wait, sizeof(wait));
    if (!recv_all(s, &ready, sizeof(ready)) || ready != 0x59445249u /* IRDY */) {
        set_message(r, ASB_GUEST_NO_CHANNEL,
                    "The guest's input helper did not answer: it is serving another connection (the VM's "
                    "window on this PC) or it is not running yet.");
        closesocket(s);
        return r->result;
    }
    for (i = 0; i < count; i++) {
        if (!send_all(s, p + (size_t)i * 20, 20)) {
            set_message(r, ASB_GUEST_IO, "The connection to the guest's input helper broke.");
            break;
        }
        if (gap_ms) Sleep(gap_ms);
    }
    Sleep(150);   /* let the helper take the last packets before the close */
    closesocket(s);
    return r->result;
}

ASB_API void asb_guest_free(AsbGuestResult *r)
{
    if (!r) return;
    free(r->out); free(r->err);
    r->out = r->err = NULL;
    r->out_len = r->err_len = 0;
}
