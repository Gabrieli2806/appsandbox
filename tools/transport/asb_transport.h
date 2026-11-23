/*
 * asb_transport.h — AppSandbox guest transport: Hyper-V sockets (AF_HYPERV / HCS hvsocket).
 *
 * Every service has its own channel; each guest sub-program (its own process) listens on its
 * channel and the host connects to it.
 *
 * No third-party code.
 */
#ifndef ASB_TRANSPORT_H
#define ASB_TRANSPORT_H

#include <stdint.h>
#include "asb_atomics.h"   /* force _Interlocked* INLINE on MSVC (/MT outlined-atomics crash fix); no-op off MSVC */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Channels (the N in the service GUID a5b0cafe-000N-4000-8000-000000000001) ---- */
#define ASB_CH_AGENT            1
#define ASB_CH_DISPLAY          2
#define ASB_CH_INPUT            3
#define ASB_CH_AUDIO            4
#define ASB_CH_CLIPBOARD        5
#define ASB_CH_CLIPBOARD_READER 6
#define ASB_CH_SSH              7

/* ================================================================================
 * Opaque handles + API (drop-in for the socket sites).
 * ================================================================================ */
typedef struct AsbListener AsbListener;
typedef struct AsbConn     AsbConn;

/* Init the transport (WSAStartup). Returns 0 on success. */
int  asb_transport_init(void);

/* Stream channels (replaces socket+bind+listen / select+accept / recv / send / close). */
AsbListener *asb_listen(int channel);
AsbConn     *asb_accept(AsbListener *l, int timeout_ms);   /* NULL on timeout */
int          asb_recv(AsbConn *c, void *buf, int len);     /* blocking; <=0 = closed */
int          asb_send(AsbConn *c, const void *buf, int len);
/* Wait up to timeout_ms for inbound data (replaces select() in the agent heartbeat loop):
 * >0 = data ready, 0 = timeout, <0 = closed. timeout_ms<0 = block. */
int          asb_poll(AsbConn *c, int timeout_ms);
/* setsockopt SO_RCVTIMEO/SO_SNDTIMEO. */
void         asb_set_timeout(AsbConn *c, int recv_ms, int send_ms);
void         asb_close(AsbConn *c);
void         asb_close_listener(AsbListener *l);

/* Newline-framed helpers (preserve the existing recv_line/send_line semantics). */
int          asb_recv_line(AsbConn *c, char *buf, int buf_size);
int          asb_send_line(AsbConn *c, const char *msg);

/* The underlying socket for a connection (for code that selects on it together with another
 * socket, e.g. the SSH relay), or (unsigned long long)(~0) for a NULL connection. */
unsigned long long asb_conn_socket_u64(AsbConn *c);

#ifdef __cplusplus
}
#endif

#endif /* ASB_TRANSPORT_H */
