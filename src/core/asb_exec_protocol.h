/*
 * asb_exec_protocol.h -- the guest command channel (channel 8): run a command
 * in the guest, write a file into it, read a file out of it. Used by the
 * host's MCP tools (run_command, copy_to_guest, copy_from_guest).
 *
 * Transport: Hyper-V socket service a5b0cafe-0008-4000-8000-000000000001 on
 * Windows guests, AF_VSOCK port 8 on Linux guests. The guest agent listens,
 * the host connects; one request and one response per connection, then both
 * sides close. Requests run in parallel (one connection each).
 *
 * Compatibility (AGENTS.md, rule 5.1):
 *  - A guest agent older than this channel does not listen on it, so the
 *    host's connect fails and the host reports "update the guest components".
 *  - An older host never connects, so the listener just sits idle.
 *  - Every message is little-endian and starts with a magic and a version.
 *    Never change the layout of version 1; add operations (unknown ones are
 *    answered with ASX_STATUS_UNSUPPORTED) or bump the version.
 *
 * Mirrored by hand in tools/linux/agent/appsandbox-agent.c (Linux guests
 * build from that single file).
 */
#ifndef ASB_EXEC_PROTOCOL_H
#define ASB_EXEC_PROTOCOL_H

#include <stdint.h>

#define ASB_EXEC_CHANNEL     8
#define ASX_REQUEST_MAGIC    0x51585341u  /* "ASXQ" little-endian */
#define ASX_RESPONSE_MAGIC   0x52585341u  /* "ASXR" little-endian */
#define ASX_VERSION          1u

/* Operations. */
#define ASX_OP_PING          1u  /* -> text = "windows" or "linux", then the agent's extras, space
                                    separated: "desktop" = it knows ASX_EXEC_DESKTOP,
                                    "user" = it knows ASX_EXEC_AS_USER / ASX_EXEC_ELEVATED */
#define ASX_OP_EXEC          2u  /* a = command, b = working directory (may be empty) */
#define ASX_OP_WRITE_FILE    3u  /* a = guest path, then data_len bytes of content */
#define ASX_OP_READ_FILE     4u  /* a = guest path; data_len = largest size accepted */

/* Request flags. */
#define ASX_EXEC_SHELL       0x1u  /* EXEC: run through the system shell (cmd.exe /c, /bin/sh -c);
                                      without it the command line is started as it is (Windows) */

#define ASX_EXEC_DESKTOP     0x2u  /* EXEC, Windows guests: start the process in the signed-in user's
                                      desktop session (still as SYSTEM), so it can show windows and
                                      read the screen. Agents that do not announce "desktop" in their
                                      PING answer ignore the flag: ask first. */

#define ASX_EXEC_AS_USER     0x4u  /* EXEC, Windows guests: run as the signed-in user, in their
                                      session and with their environment (not as SYSTEM). Programs it
                                      starts are left running when the command ends. Needs "user". */
#define ASX_EXEC_ELEVATED    0x8u  /* with ASX_EXEC_AS_USER: the user's elevated (administrator)
                                      token when they have one */

/* The agent also says what it can do on the control channel (1), right after
   "hello": "caps:" followed by space-separated words, "exec" = it has this
   channel. A host that sees it knows that a refused connection means "not
   listening yet" and not "too old". Older hosts ignore the line. */

/* Response flags. */
#define ASX_FLAG_TIMED_OUT      0x1u  /* EXEC: the time limit passed and the process was ended */
#define ASX_FLAG_OUT_TRUNCATED  0x2u  /* EXEC: stdout was longer than the limit */
#define ASX_FLAG_ERR_TRUNCATED  0x4u  /* EXEC: stderr was longer than the limit */

/* Response status: 0 = done; otherwise the operation did not happen and
   `text` says why. Values above ASX_STATUS_OS_ERROR - 1 carry no meaning
   beyond "failed"; os_error holds the guest's error code (GetLastError or
   errno). */
#define ASX_STATUS_OK           0u
#define ASX_STATUS_BAD_REQUEST  1u
#define ASX_STATUS_UNSUPPORTED  2u
#define ASX_STATUS_TOO_LARGE    3u  /* READ_FILE: the file is larger than data_len; total = its size */
#define ASX_STATUS_OS_ERROR     4u

#define ASX_MAX_STRING       32768u          /* longest a or b, in bytes */
#define ASX_MAX_OUTPUT       (4u * 1024 * 1024)   /* EXEC: largest stdout or stderr kept */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       /* ASX_REQUEST_MAGIC */
    uint32_t version;     /* ASX_VERSION */
    uint32_t op;          /* ASX_OP_* */
    uint32_t flags;
    uint32_t timeout_ms;  /* EXEC: time limit (0 = the guest's default, 60 s) */
    uint32_t limit;       /* EXEC: bytes of stdout and of stderr to keep */
    uint32_t a_len;       /* UTF-8, no terminator */
    uint32_t b_len;       /* UTF-8, no terminator */
    uint64_t data_len;    /* WRITE_FILE: bytes that follow b; READ_FILE: size limit */
} AsxRequest;             /* followed by a, b, data */

typedef struct {
    uint32_t magic;       /* ASX_RESPONSE_MAGIC */
    uint32_t version;     /* ASX_VERSION */
    uint32_t status;      /* ASX_STATUS_* */
    uint32_t os_error;    /* guest error code when status != 0 */
    uint32_t exit_code;   /* EXEC */
    uint32_t flags;       /* ASX_FLAG_* */
    uint32_t text_len;    /* UTF-8 message (error text, or PING's answer) */
    uint32_t out_len;     /* EXEC: stdout bytes */
    uint32_t err_len;     /* EXEC: stderr bytes */
    uint32_t reserved;
    uint64_t total;       /* READ_FILE: file bytes that follow (status 0) or the file's size (TOO_LARGE) */
} AsxResponse;            /* followed by text, out, err, then `total` file bytes for READ_FILE */
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(AsxRequest) == 40, "ASXQ size");
static_assert(sizeof(AsxResponse) == 48, "ASXR size");
#else
typedef char asb_check_asxq[sizeof(AsxRequest) == 40 ? 1 : -1];
typedef char asb_check_asxr[sizeof(AsxResponse) == 48 ? 1 : -1];
#endif

#endif /* ASB_EXEC_PROTOCOL_H */
