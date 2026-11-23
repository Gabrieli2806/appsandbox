#ifndef MCP_H
#define MCP_H

#include <windows.h>

/*
 * MCP (Model Context Protocol) server for AI agents: POST /mcp on
 * 127.0.0.1:47841, Streamable HTTP without sessions, served from the local
 * API's http.sys request queue (headless.c). Off until the user turns it on
 * in Settings > AI agents (MCP). The contract every app's MCP endpoint
 * follows is in ..\..\..\CLAUDE.md ("MCP"); AGENTS.md section 9 describes
 * this implementation.
 */

#define MCP_SERVER_NAME "appsandbox"
#ifndef MCP_PORT   /* a test build may listen elsewhere */
#define MCP_PORT        47841
#define MCP_PORT_W      L"47841"
#define MCP_URL         "http://127.0.0.1:47841/mcp"
#endif

/* Called by the local API (headless.c). request is a PHTTP_REQUEST. */
BOOL mcp_owns_request(const void *request);   /* it arrived on the MCP port */
void mcp_handle_http(const void *request);    /* checks it, then answers on a worker thread */
void mcp_start(void);                         /* listen if the saved setting says so */
BOOL mcp_stop(DWORD wait_ms);                 /* TRUE once no call is in flight (or after 5 s) */

/* Settings. Take effect at once and are saved (vms.cfg, [Settings]).
   persist = FALSE changes them for this run only (--mcp on the command line). */
void mcp_set_enabled(BOOL enabled, BOOL persist);
void mcp_set_read_only(BOOL read_only, BOOL persist);

/* State for the Settings page, as a UTF-8 JSON object (free() it):
   {enabled, readOnly, listening, name, url, error, lastClient,
    activity: [{time, tool, vm, ok}] newest first}. */
char *mcp_status_json(void);

#endif /* MCP_H */
