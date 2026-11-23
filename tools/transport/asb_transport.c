/*
 * asb_transport.c — AppSandbox guest transport over Hyper-V sockets (AF_HYPERV).
 * See asb_transport.h. No third-party code.
 *
 * Build: part of each guest binary (agent, the 4 channel EXEs, p9copy, vdd). Links ws2_32.
 */
#include "asb_transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN     /* keep windows.h from pulling in the old winsock.h */
#endif
#include <winsock2.h>          /* must precede windows.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- AF_HYPERV definitions, matching agent.c ---- */
#define AF_HYPERV 34
typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT         Reserved;
    GUID           VmId;
    GUID           ServiceId;
} SOCKADDR_HV;
static const GUID HV_GUID_WILDCARD = { 0,0,0,{0,0,0,0,0,0,0,0} };

/* channel -> a5b0cafe-000N-4000-8000-000000000001 (N = channel, low 16 bits). */
static GUID hv_service_guid(int channel) {
    GUID g = { 0xa5b0cafe, 0x0000, 0x4000, { 0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01 } };
    g.Data2 = (unsigned short)channel;
    return g;
}

/* ================================================================================
 * Handles.
 * ================================================================================ */
struct AsbListener {
    int       channel;
    SOCKET    sock;
};
struct AsbConn {
    int       channel;
    SOCKET    sock;
};

/* ================================================================================
 * init.
 * ================================================================================ */
int asb_transport_init(void) {
    static int inited = 0;
    WSADATA wsa;
    if (inited) return 0;        /* idempotent: multiple callers in one process (agent + p9copy) */
    inited = 1;
    WSAStartup(MAKEWORD(2, 2), &wsa);   /* Hyper-V sockets and the ssh/9p TCP relays */
    return 0;
}

/* ================================================================================
 * Stream API.
 * ================================================================================ */
AsbListener *asb_listen(int channel) {
    AsbListener *l = (AsbListener *)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->channel = channel;
    l->sock = INVALID_SOCKET;
    {   /* socket + bind(GUID) + listen. Retry bind 10x/500ms — matches the channel
           EXEs' original behaviour (the service GUID can be briefly held after a respawn). */
        SOCKADDR_HV addr; int tries; int bound = 0;
        l->sock = socket(AF_HYPERV, SOCK_STREAM, 1);
        if (l->sock == INVALID_SOCKET) { free(l); return NULL; }
        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV; addr.VmId = HV_GUID_WILDCARD; addr.ServiceId = hv_service_guid(channel);
        for (tries = 0; tries < 10; tries++) {
            if (bind(l->sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) { bound = 1; break; }
            Sleep(500);
        }
        if (!bound || listen(l->sock, 4) != 0) {
            closesocket(l->sock); free(l); return NULL;
        }
        return l;
    }
}

AsbConn *asb_accept(AsbListener *l, int timeout_ms) {
    if (!l) return NULL;
    {   /* select + accept */
        fd_set fds; struct timeval tv; SOCKET cs;
        FD_ZERO(&fds); FD_SET(l->sock, &fds);
        tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, &fds, NULL, NULL, timeout_ms < 0 ? NULL : &tv) <= 0) return NULL;
        cs = accept(l->sock, NULL, NULL);
        if (cs == INVALID_SOCKET) return NULL;
        { AsbConn *c = (AsbConn *)calloc(1, sizeof(*c)); if (!c) { closesocket(cs); return NULL; } c->channel = l->channel; c->sock = cs; return c; }
    }
}

unsigned long long asb_conn_socket_u64(AsbConn *c) {
    if (!c) return ~0ull;   /* INVALID_SOCKET */
    return (unsigned long long)c->sock;
}

int asb_send(AsbConn *c, const void *buf, int len) {
    if (!c) return -1;
    {   /* loop until all bytes are sent (a single send() can return < len on a
           stream socket) — preserves the original send_all() behaviour. */
        int off = 0;
        while (off < len) {
            int n = send(c->sock, (const char *)buf + off, len - off, 0);
            if (n <= 0) return off ? off : n;
            off += n;
        }
        return off;
    }
}

int asb_recv(AsbConn *c, void *buf, int len) {
    if (!c) return -1;
    return recv(c->sock, (char *)buf, len, 0);
}

int asb_poll(AsbConn *c, int timeout_ms) {
    if (!c) return -1;
    {   /* select on the socket */
        fd_set f; struct timeval tv;
        FD_ZERO(&f); FD_SET(c->sock, &f);
        tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        return select(0, &f, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    }
}

void asb_set_timeout(AsbConn *c, int recv_ms, int send_ms) {
    if (!c) return;
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_ms, sizeof(recv_ms));
    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&send_ms, sizeof(send_ms));
}

void asb_close(AsbConn *c) {
    if (!c) return;
    if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    free(c);
}

void asb_close_listener(AsbListener *l) {
    if (!l) return;
    if (l->sock != INVALID_SOCKET) closesocket(l->sock);
    free(l);
}

/* ---- newline-framed helpers (== recv_line/send_line in agent.c) ---- */
int asb_recv_line(AsbConn *c, char *buf, int buf_size) {
    int pos = 0;
    while (pos < buf_size - 1) {
        char ch; int n = asb_recv(c, &ch, 1);
        if (n <= 0) return n;
        if (ch == '\n') break;
        if (ch != '\r') buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}
int asb_send_line(AsbConn *c, const char *msg) {
    int len = (int)strlen(msg), n = asb_send(c, msg, len);
    if (n > 0) n = asb_send(c, "\n", 1);
    return n;
}
