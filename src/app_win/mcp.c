/*
 * mcp.c -- MCP (Model Context Protocol) server for AI agents.
 *
 * POST http://127.0.0.1:47841/mcp, one JSON-RPC message per request, answered
 * with application/json (no SSE, no sessions). The address is served by the
 * local API's http.sys request queue (headless.c); this file checks each
 * request, speaks JSON-RPC and implements the tools.
 *
 * The lifecycle tools are thin calls over the REST routes in headless.c
 * (asb_api_call), so every operation has one implementation and the same
 * validation. The guest tools (run_command, copy_to_guest, copy_from_guest,
 * get_guest_crash) use the guest command channel (vm_exec.c); screenshot uses
 * the display frame channel (the open window's picture, or asb_display.c).
 *
 * Each request is answered on its own thread: a long tool (wait_for_vm,
 * run_command) never holds up the window thread or other requests.
 *
 * Adding a tool: write a tool_xxx function, add a row to g_tools (name,
 * title, description for the model, input schema, annotations, whether it
 * changes anything) and mention it in g_instructions if it is part of a flow.
 */

#define _CRT_RAND_S
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)   /* <http.h> uses nameless structs/unions */
#include <http.h>
#pragma warning(pop)
#include <winsafer.h>
#include <shlobj.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mcp.h"
#include "headless.h"
#include "ui.h"
#include "asb_core.h"
#include "asb_display.h"
#include "asb_paths.h"
#include "vm_exec.h"
#include "protocol.h"      /* input packets, for send_input */
#include "mcp_scripts.h"   /* the PowerShell scripts the driver tools run in a guest */

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

#define MCP_MAX_BODY        (8u * 1024 * 1024)   /* request body, incl. base64 file content */
#define MCP_MAX_WORKERS     32                   /* requests answered at the same time */
#define MCP_MAX_JSON_NODES  200000               /* values in one request */
#define MCP_ACTIVITY        50
#define MCP_LATEST_LEGACY   "2025-11-25"
#define MCP_MODERN          "2026-07-28"
#define MCP_META_VERSION    "io.modelcontextprotocol/protocolVersion"
#define MCP_META_CLIENT     "io.modelcontextprotocol/clientInfo"
#define MCP_META_SERVER     "io.modelcontextprotocol/serverInfo"

static const char *const g_legacy_versions[] = { "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25" };

/* ---------------------------------------------------------------- text */

/* Growable UTF-8 buffer. A failed allocation sets `failed`; later appends
   are ignored and the caller checks once at the end. */
typedef struct {
    char  *p;
    size_t len, cap;
    BOOL   failed;
} Sb;

static void sb_put(Sb *sb, const char *data, size_t n)
{
    if (sb->failed) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 1024;
        char *grown;
        while (cap < sb->len + n + 1) cap *= 2;
        grown = (char *)realloc(sb->p, cap);
        if (!grown) { sb->failed = TRUE; return; }
        sb->p = grown; sb->cap = cap;
    }
    memcpy(sb->p + sb->len, data, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
}

static void sb_puts(Sb *sb, const char *s) { sb_put(sb, s, strlen(s)); }

static void sb_printf(Sb *sb, const char *fmt, ...)
{
    char fixed[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf_s(fixed, sizeof(fixed), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n >= 0) { sb_put(sb, fixed, (size_t)n); return; }
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n > 0) {
        char *big = (char *)malloc((size_t)n + 1);
        if (!big) { sb->failed = TRUE; return; }
        va_start(ap, fmt);
        vsprintf_s(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sb_put(sb, big, (size_t)n);
        free(big);
    }
}

static void sb_free(Sb *sb) { free(sb->p); sb->p = NULL; sb->len = sb->cap = 0; }

/* Length of the valid UTF-8 sequence at s (n bytes left), 0 if it is not one. */
static int utf8_seq(const unsigned char *s, size_t n)
{
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (n >= 2 && (s[1] & 0xC0) == 0x80) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;          /* overlong */
        if (c == 0xED && s[1] >= 0xA0) return 0;         /* surrogates */
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;
        if (c == 0xF4 && s[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

/* Append s (n bytes) as a JSON string. Bytes that are not valid UTF-8 (a
   guest program's output can be anything) become U+FFFD, so the answer is
   always valid JSON. */
static void sb_json_n(Sb *sb, const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, run = 0;
    sb_put(sb, "\"", 1);
    while (i < n) {
        unsigned char c = u[i];
        int seq = (c >= 0x20 && c != '"' && c != '\\' && c != 0x7F) ? utf8_seq(u + i, n - i) : 0;
        if (seq) { i += (size_t)seq; run += (size_t)seq; continue; }
        if (run) { sb_put(sb, (const char *)u + i - run, run); run = 0; }
        switch (c) {
        case '"':  sb_put(sb, "\\\"", 2); break;
        case '\\': sb_put(sb, "\\\\", 2); break;
        case '\n': sb_put(sb, "\\n", 2); break;
        case '\r': sb_put(sb, "\\r", 2); break;
        case '\t': sb_put(sb, "\\t", 2); break;
        default:
            if (c < 0x20 || c == 0x7F) sb_printf(sb, "\\u%04x", c);
            else sb_put(sb, "\\ufffd", 6);
        }
        i++;
    }
    if (run) sb_put(sb, (const char *)u + i - run, run);
    sb_put(sb, "\"", 1);
}

static void sb_json(Sb *sb, const char *s) { sb_json_n(sb, s ? s : "", s ? strlen(s) : 0); }

static void sb_json_w(Sb *sb, const wchar_t *w)
{
    char utf8[2048];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof(utf8), NULL, NULL)) utf8[0] = '\0';
    sb_json(sb, utf8);
}

static wchar_t *to_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

static const char g_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void sb_base64(Sb *sb, const BYTE *data, size_t n)
{
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        char q[4];
        q[0] = g_b64[data[i] >> 2];
        q[1] = g_b64[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
        q[2] = g_b64[((data[i + 1] & 15) << 2) | (data[i + 2] >> 6)];
        q[3] = g_b64[data[i + 2] & 63];
        sb_put(sb, q, 4);
    }
    if (i < n) {
        char q[4];
        BYTE b1 = i + 1 < n ? data[i + 1] : 0;
        q[0] = g_b64[data[i] >> 2];
        q[1] = g_b64[((data[i] & 3) << 4) | (b1 >> 4)];
        q[2] = i + 1 < n ? g_b64[(b1 & 15) << 2] : '=';
        q[3] = '=';
        sb_put(sb, q, 4);
    }
}

/* Decode base64 (standard or URL alphabet, white space ignored). Returns a
   malloc'd buffer, NULL when the text is not base64. */
static BYTE *base64_decode(const char *text, size_t *out_len)
{
    size_t n = strlen(text), o = 0;
    BYTE *out = (BYTE *)malloc(n / 4 * 3 + 4);
    unsigned acc = 0;
    int bits = 0;
    const char *p;
    if (!out) return NULL;
    for (p = text; *p; p++) {
        int v;
        char c = *p;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        if (c == '=') break;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else { free(out); return NULL; }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (BYTE)(acc >> bits); }
    }
    *out_len = o;
    return out;
}

/* ---------------------------------------------------------------- JSON reader */

/* A small DOM reader: MCP requests nest ({"params":{"name":..,"arguments":
   {..}}}), which the flat key scanners in webview2_bridge.c cannot tell
   apart (they would find an "name" inside "arguments"). */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType        type;
    BOOL         b;
    double       num;
    char        *str;     /* J_STR: decoded UTF-8; J_NUM: the number as written */
    size_t       len;
    char        *key;     /* member name when inside an object */
    struct JVal *child;   /* first element / member */
    struct JVal *next;
} JVal;

typedef struct { const char *p, *end; int depth; int nodes; } JReader;

static void json_free(JVal *v)
{
    while (v) {
        JVal *next = v->next;
        json_free(v->child);
        free(v->str); free(v->key);
        free(v);
        v = next;
    }
}

static void j_ws(JReader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')) r->p++;
}

static int j_hex4(const char *p)
{
    int i, v = 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* Reads a string (r->p at the opening quote) into a malloc'd buffer. */
static char *j_string(JReader *r, size_t *out_len)
{
    size_t cap = (size_t)(r->end - r->p) + 1, o = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    r->p++;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p;
        if (c < 0x20) goto bad;
        if (c != '\\') { out[o++] = (char)c; r->p++; continue; }
        if (r->end - r->p < 2) goto bad;
        r->p++;
        switch (*r->p++) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            int cp, low;
            if (r->end - r->p < 4 || (cp = j_hex4(r->p)) < 0) goto bad;
            r->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (r->end - r->p < 6 || r->p[0] != '\\' || r->p[1] != 'u' ||
                    (low = j_hex4(r->p + 2)) < 0xDC00 || low > 0xDFFF) goto bad;
                r->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) goto bad;
            /* An escape is at least as long as the bytes it becomes. */
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: goto bad;
        }
    }
    if (r->p >= r->end) goto bad;
    r->p++;
    out[o] = '\0';
    *out_len = o;
    return out;
bad:
    free(out);
    return NULL;
}

static JVal *j_value(JReader *r)
{
    JVal *v;
    j_ws(r);
    /* Limits: a request cannot make the reader recurse deeply or allocate
       without bound (a few bytes of "[1,1,1..." cost a node each). */
    if (r->p >= r->end || r->depth > 64 || ++r->nodes > MCP_MAX_JSON_NODES) return NULL;
    v = (JVal *)calloc(1, sizeof(JVal));
    if (!v) return NULL;
    if (*r->p == '{' || *r->p == '[') {
        BOOL object = *r->p == '{';
        char close = object ? '}' : ']';
        JVal **tail = &v->child;
        v->type = object ? J_OBJ : J_ARR;
        r->p++; r->depth++;
        j_ws(r);
        if (r->p < r->end && *r->p == close) { r->p++; r->depth--; return v; }
        for (;;) {
            JVal *item;
            char *key = NULL;
            j_ws(r);
            if (object) {
                size_t klen;
                if (r->p >= r->end || *r->p != '"' || (key = j_string(r, &klen)) == NULL) goto bad;
                j_ws(r);
                if (r->p >= r->end || *r->p != ':') { free(key); goto bad; }
                r->p++;
            }
            item = j_value(r);
            if (!item) { free(key); goto bad; }
            item->key = key;
            *tail = item; tail = &item->next;
            j_ws(r);
            if (r->p >= r->end) goto bad;
            if (*r->p == ',') { r->p++; continue; }
            if (*r->p == close) { r->p++; break; }
            goto bad;
        }
        r->depth--;
        return v;
    }
    if (*r->p == '"') {
        v->type = J_STR;
        v->str = j_string(r, &v->len);
        if (!v->str) goto bad;
        return v;
    }
    if (r->end - r->p >= 4 && !memcmp(r->p, "true", 4)) { v->type = J_BOOL; v->b = TRUE; r->p += 4; return v; }
    if (r->end - r->p >= 5 && !memcmp(r->p, "false", 5)) { v->type = J_BOOL; r->p += 5; return v; }
    if (r->end - r->p >= 4 && !memcmp(r->p, "null", 4)) { v->type = J_NULL; r->p += 4; return v; }
    {
        const char *start = r->p;
        size_t n;
        while (r->p < r->end && (*r->p == '-' || *r->p == '+' || *r->p == '.' || *r->p == 'e' || *r->p == 'E' ||
                                 (*r->p >= '0' && *r->p <= '9'))) r->p++;
        n = (size_t)(r->p - start);
        if (n == 0 || n > 63 || (start[0] != '-' && (start[0] < '0' || start[0] > '9'))) goto bad;
        v->type = J_NUM;
        v->str = (char *)malloc(n + 1);
        if (!v->str) goto bad;
        memcpy(v->str, start, n); v->str[n] = '\0';
        v->len = n;
        v->num = strtod(v->str, NULL);
        return v;
    }
bad:
    json_free(v);
    return NULL;
}

/* The whole text must be one JSON value. */
static JVal *json_parse(const char *text, size_t len)
{
    JReader r;
    JVal *v;
    r.p = text; r.end = text + len; r.depth = 0; r.nodes = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) r.p += 3;
    v = j_value(&r);
    if (!v) return NULL;
    j_ws(&r);
    if (r.p != r.end) { json_free(v); return NULL; }
    return v;
}

static const JVal *j_get(const JVal *obj, const char *key)
{
    const JVal *m;
    if (!obj || obj->type != J_OBJ) return NULL;
    for (m = obj->child; m; m = m->next)
        if (m->key && strcmp(m->key, key) == 0) return m;
    return NULL;
}

/* A string member without NUL bytes, or NULL. */
static const char *j_str(const JVal *obj, const char *key)
{
    const JVal *m = j_get(obj, key);
    return (m && m->type == J_STR && strlen(m->str) == m->len) ? m->str : NULL;
}

static BOOL j_int(const JVal *obj, const char *key, int *out)
{
    const JVal *m = j_get(obj, key);
    if (!m || m->type != J_NUM || m->num < -2147483647.0 || m->num > 2147483647.0 ||
        m->num != (double)(int)m->num) return FALSE;
    *out = (int)m->num;
    return TRUE;
}

static BOOL j_bool(const JVal *obj, const char *key, BOOL *out)
{
    const JVal *m = j_get(obj, key);
    if (!m || m->type != J_BOOL) return FALSE;
    *out = m->b;
    return TRUE;
}

/* ---------------------------------------------------------------- state */

typedef struct {
    SYSTEMTIME when;
    char tool[40];
    char vm[64];
    BOOL ok;
} Activity;

static CRITICAL_SECTION g_cs;          /* everything below, and the screenshot lock */
static CRITICAL_SECTION g_shot_cs;     /* one screenshot at a time (one frame-channel reader) */
static CRITICAL_SECTION g_script_cs;   /* one writer of a guest script file at a time */
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static BOOL             g_listening;
static DWORD            g_listen_error;
static Activity         g_activity[MCP_ACTIVITY];
static unsigned         g_activity_count;      /* calls ever recorded */
static char             g_last_client[96];
static volatile LONG    g_workers;
static volatile LONG    g_stopping;
static ULONGLONG        g_stop_started;

static BOOL CALLBACK mcp_init_once(PINIT_ONCE once, PVOID param, PVOID *context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_shot_cs);
    InitializeCriticalSection(&g_script_cs);
    return TRUE;
}

static void mcp_init(void) { InitOnceExecuteOnce(&g_once, mcp_init_once, NULL, NULL); }

/* Listen or stop listening so that it matches the setting. */
static void mcp_apply(void)
{
    BOOL want = asb_get_mcp_enabled();
    EnterCriticalSection(&g_cs);
    if (want && !g_listening) {
        g_listen_error = asb_api_mcp_listen(TRUE);
        g_listening = g_listen_error == NO_ERROR;
        if (g_listening)
            ui_log(L"MCP server for AI agents is on: %S%s", MCP_URL,
                   asb_get_mcp_read_only() ? L" (read-only)" : L"");
        else
            ui_log(L"MCP server for AI agents could not start on port %d (error %lu).",
                   MCP_PORT, g_listen_error);
    } else if (!want) {
        if (g_listening) {
            asb_api_mcp_listen(FALSE);
            ui_log(L"MCP server for AI agents is off.");
        }
        g_listening = FALSE;
        g_listen_error = NO_ERROR;
    }
    LeaveCriticalSection(&g_cs);
}

void mcp_start(void)
{
    mcp_init();
    InterlockedExchange(&g_stopping, 0);
    EnterCriticalSection(&g_cs);
    g_listening = FALSE;   /* a new request queue has no MCP address yet */
    g_stop_started = 0;
    LeaveCriticalSection(&g_cs);
    mcp_apply();
}

BOOL mcp_stop(DWORD wait_ms)
{
    ULONGLONG now = GetTickCount64();
    mcp_init();
    InterlockedExchange(&g_stopping, 1);
    EnterCriticalSection(&g_cs);
    if (!g_stop_started) g_stop_started = now;
    g_listening = FALSE;
    LeaveCriticalSection(&g_cs);
    if (g_workers == 0 || now - g_stop_started > 5000) return TRUE;
    Sleep(wait_ms);
    return g_workers == 0;
}

void mcp_set_enabled(BOOL enabled, BOOL persist)
{
    mcp_init();
    asb_set_mcp_enabled(enabled);
    if (persist) asb_save();
    mcp_apply();
}

void mcp_set_read_only(BOOL read_only, BOOL persist)
{
    mcp_init();
    asb_set_mcp_read_only(read_only);
    if (persist) asb_save();
    ui_log(L"MCP server for AI agents: read-only mode %s.", read_only ? L"on" : L"off");
}

char *mcp_status_json(void)
{
    Sb sb = { 0 };
    unsigned i, shown;
    mcp_init();
    EnterCriticalSection(&g_cs);
    sb_printf(&sb, "{\"enabled\":%s,\"readOnly\":%s,\"listening\":%s,\"name\":\"%s\",\"url\":\"%s\",\"error\":",
              asb_get_mcp_enabled() ? "true" : "false", asb_get_mcp_read_only() ? "true" : "false",
              g_listening ? "true" : "false", MCP_SERVER_NAME, MCP_URL);
    if (!asb_get_mcp_enabled() || g_listening || g_listen_error == NO_ERROR)
        sb_puts(&sb, "\"\"");
    else if (g_listen_error == ERROR_SHARING_VIOLATION || g_listen_error == ERROR_ALREADY_EXISTS)
        sb_printf(&sb, "\"Port %d is in use by another program. Close it, then turn this off and on again.\"", MCP_PORT);
    else if (g_listen_error == ERROR_ACCESS_DENIED)
        sb_printf(&sb, "\"Windows refused the address (access denied, port %d).\"", MCP_PORT);
    else
        sb_printf(&sb, "\"Could not listen on port %d (error %lu).\"", MCP_PORT, g_listen_error);
    sb_puts(&sb, ",\"lastClient\":");
    sb_json(&sb, g_last_client);
    sb_puts(&sb, ",\"activity\":[");
    shown = g_activity_count < MCP_ACTIVITY ? g_activity_count : MCP_ACTIVITY;
    for (i = 0; i < shown; i++) {
        const Activity *a = &g_activity[(g_activity_count - 1 - i) % MCP_ACTIVITY];
        sb_printf(&sb, "%s{\"time\":\"%02u:%02u:%02u\",\"tool\":", i ? "," : "",
                  a->when.wHour, a->when.wMinute, a->when.wSecond);
        sb_json(&sb, a->tool);
        sb_puts(&sb, ",\"vm\":");
        sb_json(&sb, a->vm);
        sb_printf(&sb, ",\"ok\":%s}", a->ok ? "true" : "false");
    }
    sb_puts(&sb, "]}");
    LeaveCriticalSection(&g_cs);
    if (sb.failed) { sb_free(&sb); return NULL; }
    return sb.p;
}

static void note_client(const char *name)
{
    if (!name || !name[0]) return;
    EnterCriticalSection(&g_cs);
    strncpy_s(g_last_client, sizeof(g_last_client), name, _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

static void note_activity(const char *tool, const char *vm, BOOL ok)
{
    Activity *a;
    EnterCriticalSection(&g_cs);
    a = &g_activity[g_activity_count % MCP_ACTIVITY];
    GetLocalTime(&a->when);
    strncpy_s(a->tool, sizeof(a->tool), tool, _TRUNCATE);
    strncpy_s(a->vm, sizeof(a->vm), vm ? vm : "", _TRUNCATE);
    a->ok = ok;
    g_activity_count++;
    LeaveCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- tools: plumbing */

typedef struct {
    Sb   out;          /* the tool result object, without its closing brace */
    BOOL is_error;
    char vm[64];       /* the VM the call was about, for the activity list */
} ToolCall;

/* Remember which VM a call is about, for the activity list and the audit
   line in the log. The text comes from the caller: control characters (a
   line break would forge a log line) become spaces. */
static void call_vm(ToolCall *t, const char *name)
{
    size_t i;
    strncpy_s(t->vm, sizeof(t->vm), name ? name : "", _TRUNCATE);
    for (i = 0; t->vm[i]; i++)
        if ((unsigned char)t->vm[i] < 0x20 || t->vm[i] == 0x7F) t->vm[i] = ' ';
}

/* The three ways a tool answers. Each starts the result object; the caller
   closes it (and adds resultType for the stateless protocol). */
static void result_text(ToolCall *t, BOOL is_error, const char *text)
{
    t->is_error = is_error;
    t->out.len = 0;
    sb_puts(&t->out, "{\"content\":[{\"type\":\"text\",\"text\":");
    sb_json(&t->out, text);
    sb_printf(&t->out, "}],\"isError\":%s", is_error ? "true" : "false");
}

/* json is a JSON object: sent as text and as structuredContent. */
static void result_json(ToolCall *t, BOOL is_error, const char *json)
{
    t->is_error = is_error;
    t->out.len = 0;
    sb_puts(&t->out, "{\"content\":[{\"type\":\"text\",\"text\":");
    sb_json(&t->out, json);
    sb_printf(&t->out, "}],\"isError\":%s,\"structuredContent\":", is_error ? "true" : "false");
    sb_puts(&t->out, json);
}

static void result_error(ToolCall *t, const char *fmt, ...)
{
    char text[1500];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    result_text(t, TRUE, text);
}

/* REST call with UTF-8 path and body. */
static BOOL rest(int method, const char *path, const char *body, int *status, char **response)
{
    wchar_t *wpath = to_wide(path), *wbody = body ? to_wide(body) : NULL;
    BOOL ok = wpath && (!body || wbody) && asb_api_call(method, wpath, wbody, status, response);
    free(wpath); free(wbody);
    return ok;
}

/* What to do about a REST error code. */
static const char *hint_for(const char *code)
{
    if (!code) return "";
    if (!strcmp(code, "vm_running")) return " Shut it down first (shutdown_vm, then wait_for_vm with until \"stopped\").";
    if (!strcmp(code, "vm_starting")) return " Call wait_for_vm, then try again.";
    if (!strcmp(code, "vm_busy") || !strcmp(code, "vm_building")) return " Call wait_for_vm, then try again.";
    if (!strcmp(code, "not_running")) return " Start it with start_vm, then wait_for_vm.";
    if (!strcmp(code, "display_not_ready")) return " Call wait_for_vm until the VM is online, then try again.";
    if (!strcmp(code, "not_found")) return " Call list_vms to see the VMs that exist.";
    return "";
}

/* Turn a REST answer into the tool result. `next` (may be NULL) is advice
   added to a success as a "next" member. Returns TRUE on success. */
static BOOL rest_result(ToolCall *t, int status, char *response, const char *next)
{
    JVal *doc = response ? json_parse(response, strlen(response)) : NULL;
    BOOL ok = status >= 200 && status < 300 && doc && doc->type == J_OBJ;
    if (ok) {
        Sb js = { 0 };
        size_t n = strlen(response);
        while (n > 0 && response[n - 1] != '}') n--;
        if (next && n > 1) {
            sb_put(&js, response, n - 1);
            sb_puts(&js, n > 2 ? ",\"next\":" : "\"next\":");
            sb_json(&js, next);
            sb_puts(&js, "}");
        } else {
            sb_put(&js, response, n);
        }
        if (js.failed || !js.p) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
        sb_free(&js);
    } else {
        const JVal *err = j_get(doc, "error");
        const char *message = j_str(err, "message"), *code = j_str(err, "code"), *hr = j_str(err, "hr");
        if (message)
            result_error(t, "%s%s%s", message, message[0] && message[strlen(message) - 1] == '.' ? "" : ".",
                         hint_for(code));
        else if (hr)
            result_error(t, "App Sandbox could not do that (HRESULT %s). Call get_log to read why, "
                            "and get_vm to see the VM's state.", hr);
        else
            result_error(t, "App Sandbox answered with status %d. Call get_log to read why.", status);
    }
    json_free(doc);
    return ok;
}

/* One REST call whose answer is the tool's answer. */
static void rest_tool(ToolCall *t, int method, const char *path, const char *body, const char *next)
{
    int status = 0;
    char *response = NULL;
    if (!rest(method, path, body, &status, &response))
        result_error(t, "App Sandbox is closing or too busy to answer. Try again in a moment.");
    else
        rest_result(t, status, response, next);
    free(response);
}

typedef struct {
    char   id[24];
    UINT64 num_id;
    char   name[128];
    char   os[32];
    char   state[24];
    BOOL   running, agent_online, install_complete, display_open;
    int    progress;
    AsbGuestTarget target;   /* for the guest calls; filled by fill_target */
} VmRef;

static void vm_from_json(const JVal *v, VmRef *vm)
{
    const char *s;
    memset(vm, 0, sizeof(*vm));
    if ((s = j_str(v, "id")) != NULL) strncpy_s(vm->id, sizeof(vm->id), s, _TRUNCATE);
    if ((s = j_str(v, "name")) != NULL) strncpy_s(vm->name, sizeof(vm->name), s, _TRUNCATE);
    if ((s = j_str(v, "osType")) != NULL) strncpy_s(vm->os, sizeof(vm->os), s, _TRUNCATE);
    if ((s = j_str(v, "state")) != NULL) strncpy_s(vm->state, sizeof(vm->state), s, _TRUNCATE);
    j_bool(v, "running", &vm->running);
    j_bool(v, "agentOnline", &vm->agent_online);
    j_bool(v, "installComplete", &vm->install_complete);
    j_bool(v, "displayOpen", &vm->display_open);
    j_int(v, "progress", &vm->progress);
    vm->num_id = asb_vm_id_parse(vm->id);
}

/* Find the VM named by the "vm" argument (its name or its id). On failure
   the tool result says what to do and FALSE is returned. */
static BOOL resolve_vm(ToolCall *t, const JVal *args, VmRef *vm)
{
    const char *want = j_str(args, "vm");
    int status = 0;
    char *response = NULL;
    JVal *doc = NULL;
    const JVal *item;
    Sb names = { 0 };
    BOOL found = FALSE;

    if (!want || !want[0]) {
        result_error(t, "Pass \"vm\": the VM's name or id. Call list_vms to see the VMs that exist.");
        return FALSE;
    }
    call_vm(t, want);
    if (!rest(ASB_API_GET, "/v1/vms", NULL, &status, &response) || status != 200 ||
        (doc = json_parse(response, strlen(response))) == NULL) {
        result_error(t, "App Sandbox is closing or too busy to answer. Try again in a moment.");
        free(response);
        return FALSE;
    }
    item = j_get(doc, "vms");
    for (item = item && item->type == J_ARR ? item->child : NULL; item && !found; item = item->next) {
        const char *name = j_str(item, "name"), *id = j_str(item, "id");
        if ((name && _stricmp(name, want) == 0) || (id && _stricmp(id, want) == 0)) {
            vm_from_json(item, vm);
            found = vm->num_id != 0;
        } else if (name) {
            sb_printf(&names, "%s%s", names.len ? ", " : "", name);
        }
    }
    if (!found)
        result_error(t, "There is no VM called \"%s\". %s%s%s", want,
                     names.len ? "The VMs are: " : "There are no VMs yet; create one with create_vm",
                     names.len ? names.p : "", ".");
    else
        call_vm(t, vm->name);
    sb_free(&names);
    json_free(doc);
    free(response);
    return found;
}

/* Re-read one VM's state. FALSE when it no longer exists. */
static BOOL refresh_vm(VmRef *vm, char **raw)
{
    char path[64];
    int status = 0;
    char *response = NULL;
    JVal *doc;
    BOOL ok = FALSE;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s", vm->id);
    if (rest(ASB_API_GET, path, NULL, &status, &response) && status == 200 &&
        (doc = json_parse(response, strlen(response))) != NULL) {
        vm_from_json(doc, vm);
        json_free(doc);
        ok = TRUE;
    }
    if (ok && raw) *raw = response; else free(response);
    return ok;
}

/* ---------------------------------------------------------------- host log */

/* The last `max_bytes` of appsandbox.log as UTF-8 (malloc'd), starting at a
   line start. */
static char *read_log_tail(DWORD max_bytes)
{
    wchar_t path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size, pos;
    char *raw, *utf8 = NULL, *start;
    wchar_t *wide;
    DWORD want, got = 0;
    int wn, un;

    asb_log_path(path, MAX_PATH);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return NULL; }
    want = size.QuadPart > (LONGLONG)max_bytes ? max_bytes : (DWORD)size.QuadPart;
    pos.QuadPart = size.QuadPart - want;
    raw = (char *)malloc((size_t)want + 1);
    if (!raw || !SetFilePointerEx(file, pos, NULL, FILE_BEGIN) || !ReadFile(file, raw, want, &got, NULL)) got = 0;
    CloseHandle(file);
    if (!raw) return NULL;
    raw[got] = '\0';
    start = raw;
    if (pos.QuadPart > 0) {   /* drop the partial first line */
        char *nl = strchr(raw, '\n');
        start = nl ? nl + 1 : raw + got;
    }
    /* The log is written in the ANSI code page. */
    wn = MultiByteToWideChar(CP_ACP, 0, start, -1, NULL, 0);
    wide = wn > 0 ? (wchar_t *)malloc((size_t)wn * sizeof(wchar_t)) : NULL;
    if (wide) {
        MultiByteToWideChar(CP_ACP, 0, start, -1, wide, wn);
        un = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
        utf8 = un > 0 ? (char *)malloc((size_t)un) : NULL;
        if (utf8) WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, un, NULL, NULL);
        free(wide);
    }
    free(raw);
    return utf8;
}

static BOOL contains_nocase(const char *text, size_t n, const char *needle)
{
    size_t m = strlen(needle), i;
    if (!m) return TRUE;
    for (i = 0; i + m <= n; i++)
        if (_strnicmp(text + i, needle, m) == 0) return TRUE;
    return FALSE;
}

/* Append the last `max_lines` log lines, one per line; with a VM name, only
   those that mention it the way the log writes VM names ("name" or [name]).
   `crash_only` keeps only the guest agent's crash reports. Returns the
   number of lines. */
static int log_lines(Sb *out, const char *vm_name, int max_lines, BOOL crash_only)
{
    char quoted[160] = "", bracketed[160] = "";
    char *tail = read_log_tail(768 * 1024), *line, *end;
    const char **kept;
    size_t *lens;
    int count = 0, i, first;

    if (!tail) return 0;
    if (vm_name && vm_name[0]) {
        sprintf_s(quoted, sizeof(quoted), "\"%.120s\"", vm_name);
        sprintf_s(bracketed, sizeof(bracketed), "[%.120s]", vm_name);
    }
    kept = (const char **)calloc((size_t)max_lines, sizeof(*kept));
    lens = (size_t *)calloc((size_t)max_lines, sizeof(*lens));
    for (line = tail; kept && lens && *line; line = end) {
        size_t n;
        end = strchr(line, '\n');
        n = end ? (size_t)(end - line) : strlen(line);
        end = end ? end + 1 : line + n;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
        if (!n) continue;
        if (quoted[0] && !contains_nocase(line, n, quoted) && !contains_nocase(line, n, bracketed)) continue;
        if (crash_only && !contains_nocase(line, n, "crashed in its previous session") &&
            !contains_nocase(line, n, "wrote a crash dump")) continue;
        kept[count % max_lines] = line;
        lens[count % max_lines] = n > 600 ? 600 : n;
        count++;
    }
    first = count > max_lines ? count - max_lines : 0;
    for (i = first; kept && lens && i < count; i++) {
        sb_put(out, kept[i % max_lines], lens[i % max_lines]);
        sb_put(out, "\n", 1);
    }
    free((void *)kept); free(lens); free(tail);
    return count - first;
}

/* ---------------------------------------------------------------- host files */

/* Files on this PC are opened, and programs started, with the rights the
   user has without elevation: App Sandbox runs elevated, and neither an MCP
   client (any program of this user) nor a file that came out of a guest
   must get to use that. The token is the app's own with the administrator
   group and its privileges taken away (Safer "normal user") at medium
   integrity. There is no fallback: when it cannot be made, the caller
   refuses the operation. */
static BOOL plain_user_token(BOOL primary, HANDLE *out)
{
    HANDLE limited = NULL;
    SAFER_LEVEL_HANDLE level = NULL;
    SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID medium = NULL;
    BOOL ok = FALSE;

    *out = NULL;
    if (SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, &level, NULL) &&
        SaferComputeTokenFromLevel(level, NULL, &limited, 0, NULL) &&
        AllocateAndInitializeSid(&mandatory, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium)) {
        TOKEN_MANDATORY_LABEL label;
        label.Label.Sid = medium;
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        ok = SetTokenInformation(limited, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(medium)) &&
             DuplicateTokenEx(limited, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                              primary ? TokenPrimary : TokenImpersonation, out);
    }
    if (medium) FreeSid(medium);
    if (limited) CloseHandle(limited);
    if (level) SaferCloseLevel(level);
    return ok;
}

/* Make the calling thread act with those rights. Returns 1 (call
   RevertToSelf afterwards) or -1 on failure. */
static int impersonate_plain_user(void)
{
    HANDLE token = NULL;
    int result = -1;
    if (plain_user_token(FALSE, &token)) {
        if (SetThreadToken(NULL, token)) result = 1;
        CloseHandle(token);
    }
    return result;
}

static void describe_win32(DWORD err, char *out, size_t cap)
{
    char sys[300] = "";
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), sys, sizeof(sys), NULL);
    if (!n) n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                               sys, sizeof(sys), NULL);
    while (n > 0 && (sys[n - 1] == '\r' || sys[n - 1] == '\n' || sys[n - 1] == ' ' || sys[n - 1] == '.')) sys[--n] = '\0';
    _snprintf_s(out, cap, _TRUNCATE, "error %lu%s%s", err, sys[0] ? ": " : "", sys);
}

/* The folder files copied out of a VM go to: Downloads\App Sandbox\<vm>. */
static BOOL exchange_dir(const char *vm_name, wchar_t *out, size_t cap)
{
    PWSTR downloads = NULL;
    wchar_t base[MAX_PATH], name[128];
    size_t i;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &downloads)) && downloads) {
        wcsncpy_s(base, MAX_PATH, downloads, _TRUNCATE);
    } else if (!GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH - 16)) {
        if (downloads) CoTaskMemFree(downloads);
        return FALSE;
    } else {
        wcscat_s(base, MAX_PATH, L"\\Downloads");
    }
    if (downloads) CoTaskMemFree(downloads);
    if (!MultiByteToWideChar(CP_UTF8, 0, vm_name, -1, name, ARRAYSIZE(name))) return FALSE;
    for (i = 0; name[i]; i++)   /* VM names are letters, digits and hyphens; be safe anyway */
        if (wcschr(L"\\/:*?\"<>|.", name[i]) || name[i] < 32) name[i] = L'_';
    return swprintf_s(out, cap, L"%s\\App Sandbox\\%s", base, name[0] ? name : L"vm") > 0;
}

static void make_dirs(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    wchar_t *p;
    if (wcslen(dir) >= MAX_PATH) return;
    wcscpy_s(path, MAX_PATH, dir);
    for (p = path + 3; *p; p++) {
        if (*p == L'\\') { *p = L'\0'; CreateDirectoryW(path, NULL); *p = L'\\'; }
    }
    CreateDirectoryW(path, NULL);
}

/* ---------------------------------------------------------------- tools */

static void tool_list_vms(ToolCall *t, const JVal *args)
{
    (void)args;
    rest_tool(t, ASB_API_GET, "/v1/vms", NULL, NULL);
}

typedef struct {
    UINT64 id;
    BOOL   found, outdated, test_mode, idd_ready;
    char   ip[32];
    int    ssh_port;
} VmExtra;

static void vm_extra_fn(void *param)
{
    VmExtra *x = (VmExtra *)param;
    VmInstance *v = asb_find_vm_by_id(x->id);
    if (!v) return;
    x->found = TRUE;
    strncpy_s(x->ip, sizeof(x->ip), v->nat_ip, _TRUNCATE);
    x->test_mode = v->test_mode;
    x->idd_ready = v->idd_ready;
    x->outdated = _wcsicmp(v->os_type, L"Windows") == 0 && asb_vm_guest_tools_outdated((AsbVm)v);
}

static void tool_get_vm(ToolCall *t, const JVal *args)
{
    VmRef vm;
    VmExtra extra = { 0 };
    char *raw = NULL;
    Sb js = { 0 }, crash = { 0 };
    size_t n;

    if (!resolve_vm(t, args, &vm)) return;
    if (!refresh_vm(&vm, &raw)) { result_error(t, "The VM no longer exists. Call list_vms."); return; }
    extra.id = vm.num_id;
    asb_api_run(vm_extra_fn, &extra);
    n = strlen(raw);
    while (n > 0 && raw[n - 1] != '}') n--;
    sb_put(&js, raw, n ? n - 1 : 0);
    sb_puts(&js, ",\"ip\":");
    sb_json(&js, extra.ip);
    sb_printf(&js, ",\"testMode\":%s,\"displayDriverReady\":%s,\"guestComponentsOutdated\":%s",
              extra.test_mode ? "true" : "false", extra.idd_ready ? "true" : "false",
              extra.outdated ? "true" : "false");
    /* The guest agent reports a bugcheck of the previous session in the log. */
    if (log_lines(&crash, vm.name, 1, TRUE) > 0 && crash.p) {
        while (crash.len > 0 && crash.p[crash.len - 1] == '\n') crash.p[--crash.len] = '\0';
        sb_puts(&js, ",\"lastGuestCrash\":");
        sb_json(&js, crash.p);
    } else {
        sb_puts(&js, ",\"lastGuestCrash\":null");
    }
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js); sb_free(&crash);
    free(raw);
}

static void tool_get_host(ToolCall *t, const JVal *args)
{
    static const char *const paths[] = { "/v1/host", "/v1/templates", "/v1/version" };
    static const char *const keys[] = { "host", "templates", "app" };
    Sb js = { 0 };
    int i;
    (void)args;
    sb_puts(&js, "{");
    for (i = 0; i < 3; i++) {
        int status = 0;
        char *response = NULL;
        JVal *doc = NULL;
        if (!rest(ASB_API_GET, paths[i], NULL, &status, &response) || status != 200 ||
            (doc = json_parse(response, strlen(response))) == NULL) {
            free(response);
            sb_free(&js);
            result_error(t, "App Sandbox is closing or too busy to answer. Try again in a moment.");
            return;
        }
        sb_printf(&js, "%s\"%s\":", i ? "," : "", keys[i]);
        if (i == 1) {   /* {"templates":[...]} -> the array */
            const char *open = strchr(response, '['), *close = strrchr(response, ']');
            if (open && close && close > open) sb_put(&js, open, (size_t)(close - open) + 1); else sb_puts(&js, "[]");
        } else {
            sb_puts(&js, response);
        }
        json_free(doc);
        free(response);
    }
    sb_printf(&js, ",\"mcp\":{\"readOnly\":%s}}", asb_get_mcp_read_only() ? "true" : "false");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
}

static void tool_create_vm(ToolCall *t, const JVal *args)
{
    const char *name = j_str(args, "name"), *os = j_str(args, "os_type");
    const char *image = j_str(args, "image_path"), *tpl = j_str(args, "template");
    const char *user = j_str(args, "admin_user"), *pass = j_str(args, "admin_password");
    const char *network = j_str(args, "network");
    BOOL gpu = TRUE, ssh = FALSE, test_mode = FALSE;
    int iv, net_mode = 1;
    Sb body = { 0 };

    if (!name || !os || !user || !pass) {
        result_error(t, "name, os_type (\"Windows\"), admin_user and admin_password are required, "
                        "plus image_path (an installer ISO on this PC) or template (see get_host).");
        return;
    }
    call_vm(t, name);
    if (network) {
        if (!_stricmp(network, "none")) net_mode = 0;
        else if (!_stricmp(network, "nat")) net_mode = 1;
        else if (!_stricmp(network, "external")) net_mode = 2;
        else if (!_stricmp(network, "internal")) net_mode = 3;
        else { result_error(t, "network must be \"nat\", \"none\", \"external\" or \"internal\"."); return; }
    }
    j_bool(args, "gpu", &gpu);
    j_bool(args, "ssh", &ssh);
    j_bool(args, "test_mode", &test_mode);
    sb_puts(&body, "{\"name\":");        sb_json(&body, name);
    sb_puts(&body, ",\"osType\":");      sb_json(&body, os);
    sb_puts(&body, ",\"adminUser\":");   sb_json(&body, user);
    sb_puts(&body, ",\"adminPass\":");   sb_json(&body, pass);
    if (image) { sb_puts(&body, ",\"imagePath\":"); sb_json(&body, image); }
    if (tpl)   { sb_puts(&body, ",\"templateName\":"); sb_json(&body, tpl); }
    if (j_int(args, "ram_mb", &iv))    sb_printf(&body, ",\"ramMb\":%d", iv);
    if (j_int(args, "disk_gb", &iv))   sb_printf(&body, ",\"hddGb\":%d", iv);
    if (j_int(args, "cpu_cores", &iv)) sb_printf(&body, ",\"cpuCores\":%d", iv);
    sb_printf(&body, ",\"gpuMode\":%d,\"networkMode\":%d,\"testMode\":%s,\"sshEnabled\":%s,\"sshDeployKey\":%s}",
              gpu ? 1 : 0, net_mode, test_mode ? "true" : "false", ssh ? "true" : "false", ssh ? "true" : "false");
    if (body.failed || body.len >= 8000) {
        result_error(t, "The arguments are too long.");
    } else {
        rest_tool(t, ASB_API_POST, "/v1/vms", body.p,
                  "The VM is being built and then installs its operating system unattended (10 to 40 minutes). "
                  "Call wait_for_vm with this VM until it reports online; call it again while it says installing.");
    }
    if (body.p) SecureZeroMemory(body.p, body.cap);
    sb_free(&body);
}

/* POST /v1/vm-ids/{id}/{action} */
static void vm_action(ToolCall *t, const JVal *args, const char *action, const char *next)
{
    VmRef vm;
    char path[96];
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/%s", vm.id, action);
    rest_tool(t, ASB_API_POST, path, NULL, next);
}

static void tool_start_vm(ToolCall *t, const JVal *args)
{
    vm_action(t, args, "start", "The VM is starting. Call wait_for_vm until it is online before run_command. "
                                "The App Sandbox app opens the VM's window on this PC by itself once the guest's "
                                "display is up (get_vm then shows displayOpen true); close_display closes it.");
}

static void tool_shutdown_vm(ToolCall *t, const JVal *args)
{
    vm_action(t, args, "shutdown",
              "Shutdown was requested. Call wait_for_vm with until \"stopped\"; if it never stops, use stop_vm.");
}

static void tool_stop_vm(ToolCall *t, const JVal *args)
{
    vm_action(t, args, "stop", "The VM was powered off. Call wait_for_vm with until \"stopped\" before snapshots.");
}

static void tool_delete_vm(ToolCall *t, const JVal *args)
{
    vm_action(t, args, "delete", "The VM, its disk and its snapshots are being deleted.");
}

static void tool_ssh_info(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/sshInfo", vm.id);
    rest_tool(t, ASB_API_GET, path, NULL,
              "sshState: 0 pending, 1 installing, 2 ready, 3 failed, 4 ready with App Sandbox's key deployed. "
              "Connect to host:port as user. run_command needs no SSH.");
}

static void tool_open_display(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/display", vm.id);
    rest_tool(t, ASB_API_POST, path, NULL, "The VM's window is open on this PC's desktop for the user.");
}

static void tool_close_display(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/display", vm.id);
    rest_tool(t, ASB_API_DELETE, path, NULL, NULL);
}

static void tool_list_snapshots(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/snapshots", vm.id);
    rest_tool(t, ASB_API_GET, path, NULL,
              "Pass a snapshot's index to restore_snapshot or delete_snapshot. current.snapIndex is the "
              "snapshot the VM's disk is based on (-1: none, -2: the base disk).");
}

static void tool_take_snapshot(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    const char *name = j_str(args, "name");
    Sb body = { 0 };
    if (!resolve_vm(t, args, &vm)) return;
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/snapshots", vm.id);
    sb_puts(&body, "{");
    if (name && name[0]) { sb_puts(&body, "\"name\":"); sb_json(&body, name); }
    sb_puts(&body, "}");
    if (body.failed || body.len > 600) result_error(t, "The snapshot name is too long.");
    else rest_tool(t, ASB_API_POST, path, body.p,
                   "Snapshot taken. Call list_snapshots for its index, then start_vm to keep working.");
    sb_free(&body);
}

static void tool_restore_snapshot(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    const char *branch = j_str(args, "branch_name");
    int index;
    Sb body = { 0 };
    if (!resolve_vm(t, args, &vm)) return;
    if (!j_int(args, "snapshot", &index) || index < 0) {
        result_error(t, "Pass \"snapshot\": the index of the snapshot, from list_snapshots.");
        return;
    }
    if (vm.running) {
        result_error(t, "The VM is running. Shut it down first (shutdown_vm or stop_vm, then wait_for_vm with "
                        "until \"stopped\"), then call restore_snapshot again.");
        return;
    }
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/start", vm.id);
    sb_printf(&body, "{\"snapIndex\":%d", index);
    if (branch && branch[0]) { sb_puts(&body, ",\"branchName\":"); sb_json(&body, branch); }
    sb_puts(&body, "}");
    if (body.failed || body.len > 400) result_error(t, "The branch name is too long.");
    else rest_tool(t, ASB_API_POST, path, body.p,
                   "The VM is starting from the snapshot's state, on a new branch (what it had after the snapshot "
                   "stays as another branch). Call wait_for_vm until it is online.");
    sb_free(&body);
}

static void tool_delete_snapshot(ToolCall *t, const JVal *args)
{
    VmRef vm;
    char path[96];
    int index;
    if (!resolve_vm(t, args, &vm)) return;
    if (!j_int(args, "snapshot", &index) || index < 0) {
        result_error(t, "Pass \"snapshot\": the index of the snapshot, from list_snapshots.");
        return;
    }
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/snapshots/%d", vm.id, index);
    rest_tool(t, ASB_API_DELETE, path, NULL, "The snapshot and the branches made from it are deleted.");
}

static BOOL command_channel_ready(VmRef *vm);

static void tool_wait_for_vm(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *until = j_str(args, "until");
    int timeout = 90, mode = 0;   /* 0 online, 1 stopped, 2 restart */
    ULONGLONG start = GetTickCount64(), deadline;
    BOOL went_down = FALSE, reached = FALSE, gone = FALSE, channel_pending = FALSE;
    char *raw = NULL;
    const char *next;
    Sb js = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (until && !_stricmp(until, "stopped")) mode = 1;
    else if (until && !_stricmp(until, "restart")) mode = 2;
    else if (until && until[0] && _stricmp(until, "online")) {
        result_error(t, "until must be \"online\" (default), \"stopped\" or \"restart\".");
        return;
    }
    j_int(args, "timeout_seconds", &timeout);
    if (timeout < 1) timeout = 1;
    if (timeout > 100) timeout = 100;
    if (mode != 1 && !_stricmp(vm.os, "Other")) {
        result_error(t, "This is a manual-install VM (OS type Other): it has no guest agent, so it never "
                        "reports online. Use get_vm for its state and screenshot to look at it.");
        return;
    }
    if (mode != 1 && !vm.running && _stricmp(vm.state, "building") && _stricmp(vm.state, "booting")) {
        result_error(t, "The VM is stopped. Start it with start_vm, then call wait_for_vm.");
        return;
    }
    deadline = start + (ULONGLONG)timeout * 1000;
    for (;;) {
        free(raw); raw = NULL;
        if (!refresh_vm(&vm, &raw)) { gone = TRUE; break; }
        if (mode == 0) reached = !_stricmp(vm.state, "online");
        else if (mode == 1) reached = !_stricmp(vm.state, "stopped");
        else {
            if (!vm.agent_online) went_down = TRUE;
            reached = went_down && !_stricmp(vm.state, "online");
            if (!vm.running && went_down && GetTickCount64() - start > 5000) break;   /* it powered off instead */
        }
        /* "Online" is only worth reporting once commands can be run: the
           agent's command channel comes up a little after the agent. */
        if (reached && mode != 1) {
            channel_pending = !command_channel_ready(&vm);
            reached = !channel_pending;
        }
        if (reached || GetTickCount64() >= deadline || asb_api_stopping()) break;
        Sleep(1000);
    }
    if (gone) {
        result_error(t, "The VM no longer exists (it was deleted, or its creation failed). Call get_log to read why.");
        return;
    }
    if (reached)
        next = mode == 1 ? "The VM is stopped. Snapshots can be taken or restored now."
                         : "The guest agent is online: run_command, copy_to_guest and copy_from_guest work now.";
    else if (channel_pending)
        next = "The guest agent is online but its command channel is not accepting connections yet (it starts a "
               "little later, above all after a restart). Call wait_for_vm again; if this persists, get_log.";
    else if (!_stricmp(vm.state, "installing") || !_stricmp(vm.state, "building"))
        next = "Still installing; this takes 10 to 40 minutes in total. Call wait_for_vm again.";
    else if (mode == 2 && !went_down)
        next = "The guest never went offline, so it did not restart during this wait. If you expected a "
               "restart or a crash, check get_guest_crash; otherwise carry on.";
    else if (mode != 1 && !vm.running)
        next = "The VM is not running: it shut down or was stopped. Call get_log to read why, then start_vm.";
    else
        next = "Not there yet. Call wait_for_vm again; if it stays like this, get_log and screenshot show what the VM is doing.";
    {
        size_t n = strlen(raw);
        while (n > 0 && raw[n - 1] != '}') n--;
        sb_printf(&js, "{\"reached\":%s,\"commandChannelReady\":%s,\"waitedSeconds\":%d,\"state\":",
                  reached ? "true" : "false", reached && mode != 1 ? "true" : "false",
                  (int)((GetTickCount64() - start) / 1000));
        sb_json(&js, vm.state);
        sb_printf(&js, ",\"installProgress\":%d,\"next\":", vm.progress);
        sb_json(&js, next);
        sb_puts(&js, ",\"vm\":");
        sb_put(&js, raw, n);
        sb_puts(&js, "}");
    }
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
    free(raw);
}

typedef struct { UINT64 id; AsbGuestTarget *target; } TargetJob;

static void target_fn(void *param)
{
    TargetJob *job = (TargetJob *)param;
    asb_guest_target(job->id, job->target);
}

/* Copy what the guest calls need from the VM's record, in the request
   context (they then never read the core's VM table from this thread). */
static void fill_target(VmRef *vm)
{
    TargetJob job;
    job.id = vm->num_id;
    job.target = &vm->target;
    memset(&vm->target, 0, sizeof(vm->target));
    asb_api_run(target_fn, &job);
}

/* Whether commands can be run in the guest now: an agent that announced the
   command channel must also answer on it. An agent that never announced it
   (an older one) has nothing to wait for. */
static BOOL command_channel_ready(VmRef *vm)
{
    AsbGuestResult r;
    fill_target(vm);
    if (!vm->target.has_exec) {   /* the announcement follows "hello" closely: look once more */
        Sleep(700);
        fill_target(vm);
        if (!vm->target.has_exec) return TRUE;
    }
    asb_guest_ping(&vm->target, &r);
    return r.result == ASB_GUEST_OK;
}

/* The guest tools need a running VM with its agent online. */
static BOOL guest_ready(ToolCall *t, VmRef *vm)
{
    if (!_stricmp(vm->os, "Other")) {
        result_error(t, "This is a manual-install VM (OS type Other): it has no App Sandbox guest agent, so "
                        "commands and file copies are not available. screenshot still works.");
        return FALSE;
    }
    if (!vm->running) {
        result_error(t, "The VM is not running. Start it with start_vm, then wait_for_vm until it is online.");
        return FALSE;
    }
    if (!vm->agent_online) {
        result_error(t, "The VM is %s: its guest agent is not online yet. Call wait_for_vm until it is online, "
                        "then try again.", vm->state);
        return FALSE;
    }
    fill_target(vm);
    return TRUE;
}

/* A failed guest call as a tool error; the message already says what to do. */
static void guest_failure(ToolCall *t, const AsbGuestResult *r)
{
    result_error(t, "%s", r->message[0] ? r->message : "The guest agent did not answer.");
}

/* "powershell.exe ... -EncodedCommand <base64 of UTF-16LE>" for a script. */
static char *powershell_command(const char *script)
{
    static const char prefix[] = "$ProgressPreference='SilentlyContinue'; ";
    Sb full = { 0 }, cmd = { 0 };
    wchar_t *wide;
    sb_puts(&full, prefix);
    sb_puts(&full, script);
    wide = full.failed ? NULL : to_wide(full.p);
    sb_free(&full);
    if (!wide) return NULL;
    sb_puts(&cmd, "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ");
    sb_base64(&cmd, (const BYTE *)wide, wcslen(wide) * sizeof(wchar_t));
    free(wide);
    if (cmd.failed) { sb_free(&cmd); return NULL; }
    return cmd.p;
}

static void tool_run_command(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *command = j_str(args, "command"), *shell = j_str(args, "shell"), *cwd = j_str(args, "working_dir");
    int timeout = 60, max_output = 16000;
    BOOL use_shell = TRUE, desktop = FALSE, elevated = FALSE;
    char *encoded = NULL;
    AsbGuestResult r;
    Sb js = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (!command || !command[0]) { result_error(t, "Pass \"command\": the command line to run in the guest."); return; }
    if (!guest_ready(t, &vm)) return;
    j_int(args, "timeout_seconds", &timeout);
    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;
    j_int(args, "max_output_bytes", &max_output);
    if (max_output < 256) max_output = 256;
    if (max_output > 200000) max_output = 200000;
    if (shell && shell[0]) {
        if (!_stricmp(shell, "powershell")) {
            encoded = powershell_command(command);
            if (!encoded) { result_error(t, "Out of memory."); return; }
            if (strlen(encoded) > 32000) {
                free(encoded);
                result_error(t, "The PowerShell script is too long for a command line (about 12000 characters). "
                                "Write it to a .ps1 file with copy_to_guest (content_text) and run that file.");
                return;
            }
            use_shell = FALSE;
        } else if (!_stricmp(shell, "none")) {
            use_shell = FALSE;
        } else if (_stricmp(shell, "cmd")) {
            result_error(t, "shell must be \"cmd\" (default), \"powershell\" or \"none\" (start the program directly).");
            return;
        }
    }
    j_bool(args, "desktop", &desktop);
    j_bool(args, "elevated", &elevated);
    if (desktop) {
        asb_guest_ping(&vm.target, &r);
        if (r.result != ASB_GUEST_OK || !strstr(r.message, " user")) {
            free(encoded);
            if (r.result != ASB_GUEST_OK) guest_failure(t, &r);
            else result_error(t, "The guest agent in this VM is too old to run programs as the signed-in user. "
                                 "Shut the VM down and start it again so App Sandbox updates its guest components.");
            return;
        }
    }
    asb_guest_exec(&vm.target, encoded ? encoded : command, cwd,
                   (use_shell ? ASB_EXEC_SHELL : 0) |
                   (desktop ? ASB_EXEC_AS_USER | (elevated ? ASB_EXEC_ELEVATED : 0) : 0),
                   (DWORD)timeout * 1000, (DWORD)max_output, &r);
    free(encoded);
    if (r.result != ASB_GUEST_OK) { guest_failure(t, &r); asb_guest_free(&r); return; }

    sb_printf(&js, "{\"exitCode\":%ld,\"timedOut\":%s,\"stdout\":", (long)(int)r.exit_code, r.timed_out ? "true" : "false");
    sb_json_n(&js, r.out, r.out_len);
    sb_puts(&js, ",\"stderr\":");
    sb_json_n(&js, r.err, r.err_len);
    sb_printf(&js, ",\"stdoutTruncated\":%s,\"stderrTruncated\":%s", r.out_truncated ? "true" : "false",
              r.err_truncated ? "true" : "false");
    if (r.timed_out)
        sb_printf(&js, ",\"next\":\"The command was ended after %d s (timeout_seconds, at most 600). For longer work "
                       "start it in the background and poll, or raise timeout_seconds.\"", timeout);
    else if (r.out_truncated || r.err_truncated)
        sb_puts(&js, ",\"next\":\"Output was cut at max_output_bytes. Redirect it to a file in the guest and read "
                     "the part you need, or fetch the file with copy_from_guest.\"");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, r.timed_out, js.p);
    sb_free(&js);
    asb_guest_free(&r);
}

static void tool_copy_to_guest(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *guest_path = j_str(args, "guest_path"), *host_path = j_str(args, "host_path");
    const char *b64 = j_str(args, "content_base64"), *text = j_str(args, "content_text");
    AsbGuestResult r;
    HANDLE file = NULL;
    BYTE *data = NULL;
    size_t data_len = 0;
    UINT64 size = 0;
    Sb js = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (!text) text = j_str(args, "content");   /* the name agents guess */
    if (!guest_path || !guest_path[0] || ((host_path != NULL) + (b64 != NULL) + (text != NULL)) != 1) {
        result_error(t, "Pass guest_path (a full path in the guest, e.g. C:\\Drivers\\my.sys) and exactly one of "
                        "host_path (a file on this PC), content_base64 or content_text.");
        return;
    }
    if (!guest_ready(t, &vm)) return;
    if (host_path) {
        wchar_t *wpath = to_wide(host_path);
        LARGE_INTEGER li;
        DWORD err = 0, attrs;
        int imp = impersonate_plain_user();
        if (imp < 0 || !wpath) {
            free(wpath);
            result_error(t, "Could not open files with the user's own rights on this PC, so host_path is not "
                            "available. Send the content with content_base64 instead.");
            return;
        }
        attrs = GetFileAttributesW(wpath);
        file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (file == INVALID_HANDLE_VALUE) err = GetLastError();
        RevertToSelf();
        free(wpath);
        if (file == INVALID_HANDLE_VALUE || (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(file, &li)) {
            char why[400];
            describe_win32(err ? err : ERROR_DIRECTORY, why, sizeof(why));
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            result_error(t, "Cannot read %s on this PC (%s). host_path must be the full path of one file that the "
                            "user can read; copy a folder file by file, or zip it first.", host_path, why);
            return;
        }
        size = (UINT64)li.QuadPart;
        if (size > 4ull * 1024 * 1024 * 1024) {
            CloseHandle(file);
            result_error(t, "The file is larger than 4 GB, the limit for copy_to_guest.");
            return;
        }
        asb_guest_write_file(&vm.target, guest_path, file, NULL, size, &r);
        CloseHandle(file);
    } else {
        if (b64) {
            data = base64_decode(b64, &data_len);
            if (!data) { result_error(t, "content_base64 is not valid base64."); return; }
        } else {
            data_len = strlen(text);
        }
        size = data_len;
        asb_guest_write_file(&vm.target, guest_path, NULL, b64 ? (const void *)data : (const void *)text, size, &r);
        free(data);
    }
    if (r.result != ASB_GUEST_OK) { guest_failure(t, &r); return; }
    sb_puts(&js, "{\"guestPath\":");
    sb_json(&js, guest_path);
    sb_printf(&js, ",\"bytes\":%llu}", (unsigned long long)size);
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
}

/* Names Windows gives to devices whatever the folder or extension. */
static BOOL reserved_name(const wchar_t *name)
{
    static const wchar_t *const devices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
    wchar_t stem[16];
    size_t i, n = 0;
    while (name[n] && name[n] != L'.' && n < ARRAYSIZE(stem) - 1) { stem[n] = name[n]; n++; }
    stem[n] = L'\0';
    while (n > 0 && stem[n - 1] == L' ') stem[--n] = L'\0';
    for (i = 0; i < ARRAYSIZE(devices); i++)
        if (!_wcsicmp(stem, devices[i])) return TRUE;
    return n == 4 && (!_wcsnicmp(stem, L"COM", 3) || !_wcsnicmp(stem, L"LPT", 3)) && stem[3] >= L'0' && stem[3] <= L'9';
}

/* Copy one guest file into Downloads\App Sandbox\<vm> on this PC, under the
   name it has in the guest (or save_as), and never anywhere else: the name
   is reduced to a plain file name, and the folder and the file are created
   with the user's own rights, so nothing the elevated app could reach but
   the user cannot is ever written. On failure the tool result is set. */
static BOOL pull_guest_file(ToolCall *t, VmRef *vm, const char *guest_path, const char *save_as, int max_mb,
                            wchar_t *final_path, UINT64 *bytes)
{
    const char *leaf;
    wchar_t dir[MAX_PATH], part[MAX_PATH], wname[160];
    BY_HANDLE_FILE_INFORMATION info;
    int imp;
    size_t i;
    HANDLE file;
    DWORD err = 0;
    AsbGuestResult r;

    leaf = save_as && save_as[0] ? save_as : guest_path;
    for (i = strlen(leaf); i > 0 && leaf[i - 1] != '\\' && leaf[i - 1] != '/' && leaf[i - 1] != ':'; i--) {}
    if (!MultiByteToWideChar(CP_UTF8, 0, leaf + i, -1, wname, ARRAYSIZE(wname)) || !wname[0]) {
        result_error(t, "The file name is empty or longer than 150 characters; pass save_as with a shorter name.");
        return FALSE;
    }
    for (i = 0; wname[i]; i++)   /* no separators, no stream names, no wildcards */
        if (wcschr(L"\\/:*?\"<>|", wname[i]) || wname[i] < 32) wname[i] = L'_';
    while (i > 0 && (wname[i - 1] == L'.' || wname[i - 1] == L' ')) wname[--i] = L'\0';
    if (reserved_name(wname)) wname[0] = L'_';
    if (!wname[0] || !exchange_dir(vm->name, dir, MAX_PATH) ||
        swprintf_s(final_path, MAX_PATH, L"%s\\%s", dir, wname) < 0 ||
        swprintf_s(part, MAX_PATH, L"%s.part", final_path) < 0) {
        result_error(t, "Could not build the destination path on this PC; pass save_as with a short, plain name.");
        return FALSE;
    }
    imp = impersonate_plain_user();
    if (imp < 0) {
        result_error(t, "Could not create files with the user's own rights on this PC, so the file was not copied.");
        return FALSE;
    }
    make_dirs(dir);
    file = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) err = GetLastError();
    RevertToSelf();
    /* A link in the way is not followed, even with the user's rights. */
    if (file != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1)) {
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        err = ERROR_CANT_ACCESS_FILE;
    }
    if (file == INVALID_HANDLE_VALUE) {
        char why[400];
        describe_win32(err, why, sizeof(why));
        result_error(t, "Cannot create the file in the Downloads\\App Sandbox folder on this PC (%s).", why);
        return FALSE;
    }
    asb_guest_read_file(&vm->target, guest_path, file, (UINT64)max_mb * 1024 * 1024, &r);
    CloseHandle(file);

    imp = impersonate_plain_user();
    if (r.result == ASB_GUEST_OK && imp >= 0 && !MoveFileExW(part, final_path, MOVEFILE_REPLACE_EXISTING)) {
        err = GetLastError();
        r.result = ASB_GUEST_HOST_FILE;
        _snprintf_s(r.message, sizeof(r.message), _TRUNCATE,
                    "The file was copied but could not be given its final name on this PC (error %lu).", err);
    }
    if (r.result != ASB_GUEST_OK && imp >= 0) DeleteFileW(part);   /* only with the user's own rights */
    if (imp > 0) RevertToSelf();
    if (imp < 0) { result_error(t, "Could not finish the file with the user's own rights on this PC."); return FALSE; }

    if (r.result == ASB_GUEST_TOO_LARGE) {
        result_error(t, "The file is %llu MB, more than max_mb (%d). Pass a larger max_mb (up to 4096), or "
                        "compress or split it in the guest with run_command first.",
                     (unsigned long long)(r.total / (1024 * 1024)) + 1, max_mb);
        return FALSE;
    }
    if (r.result != ASB_GUEST_OK) { guest_failure(t, &r); return FALSE; }
    *bytes = r.total;
    return TRUE;
}

static void tool_copy_from_guest(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *guest_path = j_str(args, "guest_path"), *save_as = j_str(args, "save_as");
    wchar_t final_path[MAX_PATH];
    int max_mb = 256;
    UINT64 bytes = 0;
    Sb js = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (!guest_path || !guest_path[0]) {
        result_error(t, "Pass guest_path: the full path of the file in the guest, e.g. C:\\Windows\\Minidump\\x.dmp.");
        return;
    }
    if (!guest_ready(t, &vm)) return;
    j_int(args, "max_mb", &max_mb);
    if (max_mb < 1) max_mb = 1;
    if (max_mb > 4096) max_mb = 4096;
    if (!pull_guest_file(t, &vm, guest_path, save_as, max_mb, final_path, &bytes)) return;
    sb_puts(&js, "{\"hostPath\":");
    sb_json_w(&js, final_path);
    sb_puts(&js, ",\"guestPath\":");
    sb_json(&js, guest_path);
    sb_printf(&js, ",\"bytes\":%llu}", (unsigned long long)bytes);
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
}

static void tool_get_guest_crash(ToolCall *t, const JVal *args)
{
    static const char windows_script[] =
        "$ErrorActionPreference='SilentlyContinue'; "
        "$b=@(Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-WER-SystemErrorReporting';Id=1001} -MaxEvents 5 | "
        "ForEach-Object { @{time=$_.TimeCreated.ToString('s');bugcheck=[string]$_.Properties[0].Value;dump=[string]$_.Properties[1].Value} }); "
        "$k=@(Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-Kernel-Power';Id=41} -MaxEvents 5 | "
        "ForEach-Object { @{time=$_.TimeCreated.ToString('s');bugcheckCode=('0x{0:X8}' -f [uint32]$_.Properties[0].Value)} }); "
        "$d=@(Get-ChildItem \"$env:SystemRoot\\Minidump\\*.dmp\",\"$env:SystemRoot\\MEMORY.DMP\",\"$env:SystemRoot\\LiveKernelReports\\*.dmp\" | "
        "Sort-Object LastWriteTime -Descending | Select-Object -First 20 | "
        "ForEach-Object { @{path=$_.FullName;bytes=$_.Length;written=$_.LastWriteTime.ToString('s')} }); "
        "$o=Get-CimInstance Win32_OperatingSystem; "
        "@{bugchecks=$b;unexpectedShutdowns=$k;dumps=$d;lastBoot=$o.LastBootUpTime.ToString('s');now=(Get-Date).ToString('s');"
        "windowsBuild=$o.Version} | ConvertTo-Json -Depth 4 -Compress";
    VmRef vm;
    char *command;
    AsbGuestResult r;
    Sb js = { 0 }, host = { 0 };
    JVal *guest = NULL;

    if (!resolve_vm(t, args, &vm)) return;
    if (!guest_ready(t, &vm)) return;
    command = powershell_command(windows_script);
    if (!command) { result_error(t, "Out of memory."); return; }
    asb_guest_exec(&vm.target, command, NULL, 0, 60000, 60000, &r);
    free(command);
    if (r.result != ASB_GUEST_OK) { guest_failure(t, &r); asb_guest_free(&r); return; }
    while (r.out_len > 0 && (r.out[r.out_len - 1] == '\r' || r.out[r.out_len - 1] == '\n' || r.out[r.out_len - 1] == ' '))
        r.out[--r.out_len] = '\0';

    log_lines(&host, vm.name, 5, TRUE);
    sb_puts(&js, "{\"guest\":");
    if (r.out_len && (guest = json_parse(r.out, r.out_len)) != NULL && guest->type == J_OBJ)
        sb_put(&js, r.out, r.out_len);
    else {   /* PowerShell said something else */
        Sb both = { 0 };
        sb_put(&both, r.out, r.out_len);
        if (r.err_len) { sb_puts(&both, "\n"); sb_put(&both, r.err, r.err_len); }
        sb_json_n(&js, both.p ? both.p : "", both.len);
        sb_free(&both);
    }
    json_free(guest);
    while (host.len > 0 && host.p[host.len - 1] == '\n') host.p[--host.len] = '\0';
    sb_puts(&js, ",\"hostLog\":");
    sb_json(&js, host.p ? host.p : "");
    sb_puts(&js, ",\"next\":");
    sb_json(&js,
        "bugchecks lists the stop codes Windows recorded (newest first) with their four parameters; a crash "
        "newer than lastBoot cannot exist, so compare times with the moment of your test. Fetch a dump with "
        "copy_from_guest and analyse it on this PC (cdb/WinDbg: !analyze -v). No new entry and no new dump "
        "means the guest did not bugcheck.");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js); sb_free(&host);
    asb_guest_free(&r);
}

static void tool_get_log(ToolCall *t, const JVal *args)
{
    const char *vm = j_str(args, "vm");
    int lines = 80, got;
    Sb out = { 0 };
    char tag[200] = "";

    j_int(args, "lines", &lines);
    if (lines < 1) lines = 1;
    if (lines > 400) lines = 400;
    if (vm && vm[0]) {
        VmRef ref;
        if (!resolve_vm(t, args, &ref)) return;
        strncpy_s(tag, sizeof(tag), ref.name, _TRUNCATE);
    }
    got = log_lines(&out, tag, lines, FALSE);
    if (out.failed) result_error(t, "Out of memory.");
    else if (!got) result_text(t, FALSE, tag[0] ? "No recent activity mentions this VM." : "The activity log is empty.");
    else result_text(t, FALSE, out.p);
    sb_free(&out);
}

/* ---------------------------------------------------------------- programs on this PC */

/* Start a program on this PC as the user without elevation (never as the
   elevated app: it reads files that come from a guest), with its output
   captured, a time limit and a job that ends its whole process tree.
   env_name/env_value add one environment variable. FALSE (why filled in)
   when it could not be started. */
static BOOL run_plain_process(wchar_t *command_line, const wchar_t *env_name, const wchar_t *env_value,
                              DWORD timeout_ms, size_t max_output, Sb *output, DWORD *exit_code,
                              BOOL *timed_out, char *why, size_t why_cap)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE token = NULL, out_r = NULL, out_w = NULL, nul = INVALID_HANDLE_VALUE, job = NULL;
    STARTUPINFOEXW six;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL attrs_ready = FALSE, started = FALSE;
    SIZE_T attr_size = 0;
    HANDLE inherit[2];
    wchar_t *env = NULL, system_dir[MAX_PATH];
    ULONGLONG begin;

    *exit_code = 0; *timed_out = FALSE;
    ZeroMemory(&pi, sizeof(pi));
    if (!plain_user_token(TRUE, &token)) {
        _snprintf_s(why, why_cap, _TRUNCATE, "could not create a non-elevated identity to run it with");
        return FALSE;
    }
    /* The app's environment plus the extra variable. */
    {
        wchar_t *block = GetEnvironmentStringsW(), *src, *dst;
        size_t total = 2, name_len = env_name ? wcslen(env_name) : 0;
        for (src = block; src && *src; src += wcslen(src) + 1) total += wcslen(src) + 1;
        if (env_name) total += name_len + wcslen(env_value) + 2;
        env = (wchar_t *)malloc(total * sizeof(wchar_t));
        dst = env;
        for (src = block; env && src && *src; src += wcslen(src) + 1) {
            size_t n = wcslen(src);
            if (env_name && n > name_len && src[name_len] == L'=' && _wcsnicmp(src, env_name, name_len) == 0) continue;
            memcpy(dst, src, (n + 1) * sizeof(wchar_t));
            dst += n + 1;
        }
        if (env && env_name) dst += swprintf_s(dst, name_len + wcslen(env_value) + 2, L"%s=%s", env_name, env_value) + 1;
        if (env) *dst = L'\0';
        if (block) FreeEnvironmentStringsW(block);
    }
    if (!env || !CreatePipe(&out_r, &out_w, &sa, 1 << 16)) {
        _snprintf_s(why, why_cap, _TRUNCATE, "out of memory or handles");
        goto done;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    ZeroMemory(&six, sizeof(six));
    six.StartupInfo.cb = sizeof(six);
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : NULL;
    six.StartupInfo.hStdOutput = out_w;
    six.StartupInfo.hStdError = out_w;
    inherit[0] = out_w; inherit[1] = nul;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = attr_size ? (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size) : NULL;
    if (attrs) attrs_ready = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    if (!attrs_ready || !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                   (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), NULL, NULL)) {
        _snprintf_s(why, why_cap, _TRUNCATE, "could not prepare the process");
        goto done;
    }
    six.lpAttributeList = attrs;
    if (!GetSystemDirectoryW(system_dir, MAX_PATH)) system_dir[0] = L'\0';
    started = CreateProcessAsUserW(token, NULL, command_line, NULL, NULL, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                   CREATE_UNICODE_ENVIRONMENT, env, system_dir[0] ? system_dir : NULL,
                                   &six.StartupInfo, &pi);
    if (!started) {
        describe_win32(GetLastError(), why, why_cap);
        goto done;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(out_w); out_w = NULL;

    begin = GetTickCount64();
    for (;;) {
        char chunk[4096];
        DWORD avail = 0, got = 0, wait;
        while (PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (!ReadFile(out_r, chunk, avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail, &got, NULL) || !got) break;
            if (output->len < max_output) sb_put(output, chunk, got);
        }
        wait = WaitForSingleObject(pi.hProcess, 30);
        if (wait != WAIT_TIMEOUT) {
            while (PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (!ReadFile(out_r, chunk, avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail, &got, NULL) || !got) break;
                if (output->len < max_output) sb_put(output, chunk, got);
            }
            break;
        }
        if (GetTickCount64() - begin >= timeout_ms || asb_api_stopping()) {
            *timed_out = TRUE;
            if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, exit_code);
done:
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    free(attrs); free(env);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(token);
    return started;
}

static BOOL file_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* cdb.exe or kd.exe on this PC: the Debugging Tools for Windows (Windows
   SDK / WDK), the PATH, or the engine inside the WinDbg app. */
static BOOL find_host_debugger(wchar_t *out, size_t cap)
{
    static const wchar_t *const roots[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432" };
    static const wchar_t *const archs[] = { L"x64", L"arm64" };
    static const wchar_t *const exes[] = { L"cdb.exe", L"kd.exe" };
    wchar_t base[MAX_PATH], path[MAX_PATH];
    size_t r, a, e;

    for (r = 0; r < ARRAYSIZE(roots); r++) {
        if (!GetEnvironmentVariableW(roots[r], base, MAX_PATH)) continue;
        for (a = 0; a < ARRAYSIZE(archs); a++)
            for (e = 0; e < ARRAYSIZE(exes); e++)
                if (swprintf_s(path, MAX_PATH, L"%s\\Windows Kits\\10\\Debuggers\\%s\\%s", base, archs[a], exes[e]) > 0 &&
                    file_exists(path)) {
                    wcscpy_s(out, cap, path);
                    return TRUE;
                }
    }
    for (e = 0; e < ARRAYSIZE(exes); e++)
        if (SearchPathW(NULL, exes[e], NULL, MAX_PATH, path, NULL) && file_exists(path)) {
            wcscpy_s(out, cap, path);
            return TRUE;
        }
    /* The WinDbg app (Store / winget) carries the same engine. */
    {
        wchar_t command[] = L"powershell.exe -NoLogo -NoProfile -NonInteractive -Command "
                            L"\"(Get-AppxPackage Microsoft.WinDbg | Select-Object -First 1).InstallLocation\"";
        Sb found = { 0 };
        DWORD code = 0;
        BOOL timed_out = FALSE, ok = FALSE;
        char why[200];
        if (run_plain_process(command, NULL, NULL, 30000, 2000, &found, &code, &timed_out, why, sizeof(why)) &&
            found.p && !found.failed) {
            wchar_t *location = to_wide(found.p);
            size_t n = location ? wcslen(location) : 0;
            while (n > 0 && (location[n - 1] == L'\r' || location[n - 1] == L'\n' || location[n - 1] == L' ')) location[--n] = L'\0';
            for (a = 0; n && !ok && a < 2; a++)
                for (e = 0; !ok && e < ARRAYSIZE(exes); e++)
                    if (swprintf_s(path, MAX_PATH, L"%s\\%s%s", location, a ? L"" : L"amd64\\", exes[e]) > 0 &&
                        file_exists(path)) {
                        wcscpy_s(out, cap, path);
                        ok = TRUE;
                    }
            free(location);
        }
        sb_free(&found);
        return ok;
    }
}

/* ---------------------------------------------------------------- guest scripts */

static volatile LONG g_script_serial;

/* Run one of the PowerShell scripts of mcp_scripts.h in a Windows guest. The
   script and its parameters go in as files (nothing an argument holds ever
   reaches a command line); the script prints one JSON object. On failure the
   tool result is set and FALSE returned; on success free r with
   asb_guest_free. */
static BOOL guest_script(ToolCall *t, VmRef *vm, const char *name, const char *script, const char *params_json,
                         DWORD exec_flags, DWORD timeout_ms, AsbGuestResult *r)
{
    char script_path[200], param_path[200], command[700];
    LONG serial = InterlockedIncrement(&g_script_serial);

    sprintf_s(script_path, sizeof(script_path), "C:\\Windows\\Temp\\AppSandbox\\%s.ps1", name);
    sprintf_s(param_path, sizeof(param_path), "C:\\Windows\\Temp\\AppSandbox\\%s-%lu-%ld.json", name,
              (unsigned long)GetTickCount(), (long)serial);
    EnterCriticalSection(&g_script_cs);   /* one writer of the script file at a time */
    asb_guest_write_file(&vm->target, script_path, NULL, script, strlen(script), r);
    LeaveCriticalSection(&g_script_cs);
    if (r->result == ASB_GUEST_OK)
        asb_guest_write_file(&vm->target, param_path, NULL, params_json, strlen(params_json), r);
    if (r->result != ASB_GUEST_OK) { guest_failure(t, r); return FALSE; }
    sprintf_s(command, sizeof(command),
              "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\" \"%s\"",
              script_path, param_path);
    asb_guest_exec(&vm->target, command, NULL, exec_flags, timeout_ms, 400000, r);
    if (r->result != ASB_GUEST_OK) { guest_failure(t, r); asb_guest_free(r); return FALSE; }
    while (r->out_len > 0 && (r->out[r->out_len - 1] == '\r' || r->out[r->out_len - 1] == '\n' ||
                              r->out[r->out_len - 1] == ' '))
        r->out[--r->out_len] = '\0';
    return TRUE;
}

/* The JSON object a guest script printed, or NULL with the tool result set
   to what the script said instead. Free with json_free. */
static JVal *script_json(ToolCall *t, const AsbGuestResult *r)
{
    JVal *doc = r->out_len ? json_parse(r->out, r->out_len) : NULL;
    if (doc && doc->type == J_OBJ) return doc;
    json_free(doc);
    {
        Sb said = { 0 };
        const char *text;
        size_t n;
        if (r->err_len) sb_put(&said, r->err, r->err_len);
        if (r->out_len) { sb_puts(&said, "\n"); sb_put(&said, r->out, r->out_len); }
        text = said.p ? said.p : "";
        n = strlen(text);
        if (n > 900) text += n - 900;
        result_error(t, "The script in the guest did not finish (exit code %ld%s). It said: %s",
                     (long)(int)r->exit_code, r->timed_out ? ", time limit reached" : "", text[0] ? text : "nothing");
        sb_free(&said);
    }
    return NULL;
}

/* Start a JSON object from a guest script's answer, left open so members can
   be added. */
static void json_reopen(Sb *js, const AsbGuestResult *r)
{
    size_t n = r->out_len;
    while (n > 0 && r->out[n - 1] != '}') n--;
    sb_put(js, r->out, n ? n - 1 : 0);
    if (js->len < 2) { js->len = 0; sb_puts(js, "{\"ok\":false"); }
}

/* Whether the VM has at least one snapshot to go back to. */
static BOOL vm_has_snapshot(const VmRef *vm)
{
    char path[96];
    int status = 0;
    char *response = NULL;
    JVal *doc = NULL;
    const JVal *list;
    BOOL has = TRUE;   /* when in doubt, do not warn */
    sprintf_s(path, sizeof(path), "/v1/vm-ids/%s/snapshots", vm->id);
    if (rest(ASB_API_GET, path, NULL, &status, &response) && status == 200 &&
        (doc = json_parse(response, strlen(response))) != NULL) {
        list = j_get(doc, "snapshots");
        has = list && list->type == J_ARR && list->child != NULL;
    }
    json_free(doc);
    free(response);
    return has;
}

static const char g_no_snapshot[] =
    "This VM has no snapshot, so there is no way back if the driver breaks the guest. Snapshots need a stopped "
    "VM: shutdown_vm, wait_for_vm until \"stopped\", take_snapshot, start_vm.";

/* Wait until the guest's agent has gone offline and is online again. */
static BOOL wait_restart(VmRef *vm, int timeout_s, int *waited_s)
{
    ULONGLONG begin = GetTickCount64(), deadline = begin + (ULONGLONG)timeout_s * 1000;
    BOOL down = FALSE, back = FALSE;
    for (;;) {
        if (!refresh_vm(vm, NULL)) break;
        if (!vm->agent_online) down = TRUE;
        back = down && !_stricmp(vm->state, "online") && command_channel_ready(vm);
        if (back || GetTickCount64() >= deadline || asb_api_stopping()) break;
        if (!vm->running && down && GetTickCount64() - begin > 5000) break;
        Sleep(1000);
    }
    *waited_s = (int)((GetTickCount64() - begin) / 1000);
    return back;
}

/* Ask the guest OS to restart. TRUE when the request went out. */
static BOOL guest_reboot(ToolCall *t, VmRef *vm)
{
    AsbGuestResult r;
    asb_guest_exec(&vm->target, "shutdown.exe /r /t 0 /f", NULL, 0, 30000, 4000, &r);
    /* A connection that breaks here is the restart itself. */
    if (r.result != ASB_GUEST_OK && r.result != ASB_GUEST_IO) { guest_failure(t, &r); asb_guest_free(&r); return FALSE; }
    if (r.result == ASB_GUEST_OK && r.exit_code != 0) {
        result_error(t, "The guest refused to restart (exit code %ld): %.300s", (long)(int)r.exit_code,
                     r.err_len ? r.err : (r.out ? r.out : ""));
        asb_guest_free(&r);
        return FALSE;
    }
    asb_guest_free(&r);
    return TRUE;
}

static void tool_restart_vm(ToolCall *t, const JVal *args)
{
    VmRef vm;
    int timeout = 90, waited = 0;
    BOOL wait = TRUE, back = FALSE;
    Sb js = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (!guest_ready(t, &vm)) return;
    j_int(args, "timeout_seconds", &timeout);
    if (timeout < 5) timeout = 5;
    if (timeout > 100) timeout = 100;
    j_bool(args, "wait", &wait);
    if (!guest_reboot(t, &vm)) return;
    if (wait) back = wait_restart(&vm, timeout, &waited);
    sb_printf(&js, "{\"restartRequested\":true,\"backOnline\":%s,\"waitedSeconds\":%d,\"state\":", back ? "true" : "false", waited);
    sb_json(&js, vm.state);
    sb_puts(&js, ",\"next\":");
    sb_json(&js, back ? "The guest restarted; its agent is online and commands can be run again. A get_debug_output capture must be started again."
                 : wait ? "The guest is not back yet. Call wait_for_vm (until \"online\"); if it does not come back, "
                          "it may be crashing at boot: screenshot shows the screen, and stop_vm + restore_snapshot goes back."
                        : "Restart requested. Call wait_for_vm with until \"restart\".");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
}

/* A driver or service name that is safe to hand to a tool: letters, digits,
   dot, underscore, hyphen. */
static BOOL plain_name(const char *s, size_t max)
{
    size_t i;
    if (!s || !s[0] || strlen(s) > max) return FALSE;
    for (i = 0; s[i]; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '.' || s[i] == '_' || s[i] == '-')) return FALSE;
    return TRUE;
}

static void tool_set_verifier(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *preset = j_str(args, "preset");
    const JVal *drivers = j_get(args, "drivers"), *d;
    int flags = 0, count = 0, waited = 0;
    BOOL reboot = FALSE, has_flags = j_int(args, "flags", &flags), back = FALSE, ok = FALSE;
    Sb params = { 0 }, js = { 0 };
    AsbGuestResult r;
    JVal *doc;

    if (!resolve_vm(t, args, &vm)) return;
    if (!preset || !preset[0]) preset = has_flags ? "custom" : "standard";
    if (strcmp(preset, "standard") && strcmp(preset, "custom") && strcmp(preset, "reset") && strcmp(preset, "query")) {
        result_error(t, "preset must be \"standard\", \"custom\" (with flags), \"reset\" (turn Driver Verifier off) or \"query\".");
        return;
    }
    if (!strcmp(preset, "custom") && (!has_flags || flags <= 0)) {
        result_error(t, "preset \"custom\" needs flags: the sum of the Driver Verifier option bits, e.g. 2363 "
                        "(0x93B, the standard set) or 9 (special pool + pool tracking).");
        return;
    }
    j_bool(args, "reboot", &reboot);
    sb_puts(&params, "{\"preset\":");
    sb_json(&params, preset);
    sb_printf(&params, ",\"flags\":\"0x%X\",\"drivers\":[", (unsigned)flags);
    for (d = drivers && drivers->type == J_ARR ? drivers->child : NULL; d; d = d->next) {
        if (d->type != J_STR || !plain_name(d->str, 64) || count >= 32) {
            sb_free(&params);
            result_error(t, "drivers must be up to 32 driver file names such as \"mydrv.sys\" (letters, digits, dot, "
                            "underscore, hyphen; no paths).");
            return;
        }
        sb_puts(&params, count++ ? "," : "");
        sb_json(&params, d->str);
    }
    sb_puts(&params, "]}");
    if ((!strcmp(preset, "standard") || !strcmp(preset, "custom")) && !count) {
        sb_free(&params);
        result_error(t, "Pass drivers: the driver file names to verify, e.g. [\"mydrv.sys\"].");
        return;
    }
    if (!guest_ready(t, &vm)) {
        sb_free(&params);
        return;
    }
    if (params.failed || !guest_script(t, &vm, "verifier", g_ps_verifier, params.p, 0, 60000, &r)) {
        if (params.failed) result_error(t, "Out of memory.");
        sb_free(&params);
        return;
    }
    sb_free(&params);
    doc = script_json(t, &r);
    if (!doc) { asb_guest_free(&r); return; }
    j_bool(doc, "ok", &ok);
    json_reopen(&js, &r);
    if (ok && reboot && strcmp(preset, "query")) {
        ToolCall quiet;
        memset(&quiet, 0, sizeof(quiet));
        if (guest_reboot(&quiet, &vm)) back = wait_restart(&vm, 95, &waited);
        sb_free(&quiet.out);
        sb_printf(&js, ",\"restarted\":%s,\"waitedSeconds\":%d", back ? "true" : "false", waited);
    }
    if (ok && strcmp(preset, "query") && strcmp(preset, "reset") && !vm_has_snapshot(&vm)) {
        sb_puts(&js, ",\"snapshotWarning\":");
        sb_json(&js, g_no_snapshot);
    }
    sb_puts(&js, ",\"next\":");
    sb_json(&js, !ok ? "verifier.exe refused the settings; its output says why."
                 : !strcmp(preset, "query") ? "These are the settings that apply after the next restart; verifier /query (run_command) shows what is active now."
                 : reboot ? (back ? "The guest restarted with the new Driver Verifier settings. A verifier violation is a bugcheck (0xC4, 0xC9, 0xC1...): after your test call get_guest_crash and analyze_dump."
                                  : "The guest was restarted but is not back online yet: call wait_for_vm. If it crashes at boot, stop_vm and restore_snapshot.")
                 : "Driver Verifier settings change at the next restart: call restart_vm (or pass reboot true). A verifier violation is a bugcheck: get_guest_crash and analyze_dump show it.");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, !ok, js.p);
    sb_free(&js);
    json_free(doc);
    asb_guest_free(&r);
}

static void tool_get_devices(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *filter = j_str(args, "filter");
    BOOL problem_only = TRUE;
    int max = 60;
    Sb params = { 0 };
    AsbGuestResult r;
    JVal *doc;

    if (!resolve_vm(t, args, &vm)) return;
    j_bool(args, "problem_only", &problem_only);
    j_int(args, "max", &max);
    if (max < 1) max = 1;
    if (max > 200) max = 200;
    if (filter && strlen(filter) > 100) { result_error(t, "filter is longer than 100 characters."); return; }
    if (!guest_ready(t, &vm)) return;
    sb_printf(&params, "{\"problemOnly\":%s,\"max\":%d,\"filter\":", problem_only ? "true" : "false", max);
    sb_json(&params, filter ? filter : "");
    sb_puts(&params, "}");
    if (params.failed || !guest_script(t, &vm, "devices", g_ps_devices, params.p, 0, 120000, &r)) {
        if (params.failed) result_error(t, "Out of memory.");
        sb_free(&params);
        return;
    }
    sb_free(&params);
    doc = script_json(t, &r);
    if (doc) {
        Sb js = { 0 };
        json_reopen(&js, &r);
        sb_puts(&js, ",\"next\":\"problemCode is the Device Manager code (0 = working; 10 cannot start, 28 no driver, "
                     "31 driver failed to load, 39 driver corrupt or missing, 52 signature not accepted). "
                     "thirdPartyDrivers are the oemNN.inf packages in the driver store (uninstall_driver removes one).\"}");
        if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
        sb_free(&js);
    }
    json_free(doc);
    asb_guest_free(&r);
}

static void tool_uninstall_driver(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *published = j_str(args, "published_name"), *service = j_str(args, "service_name");
    Sb params = { 0 };
    AsbGuestResult r;
    JVal *doc;
    BOOL ok = FALSE;

    if (!resolve_vm(t, args, &vm)) return;
    if ((!published && !service) || (published && (!plain_name(published, 40) || _strnicmp(published, "oem", 3) != 0)) ||
        (service && !plain_name(service, 64))) {
        result_error(t, "Pass published_name (the oemNN.inf name install_driver or get_devices returned) to remove a "
                        "driver package and uninstall it from its devices, and/or service_name to stop and delete a "
                        "legacy driver service.");
        return;
    }
    if (!guest_ready(t, &vm)) return;
    sb_puts(&params, "{\"publishedName\":");
    if (published) sb_json(&params, published); else sb_puts(&params, "null");
    sb_puts(&params, ",\"service\":");
    if (service) sb_json(&params, service); else sb_puts(&params, "null");
    sb_puts(&params, "}");
    if (params.failed || !guest_script(t, &vm, "driver_uninstall", g_ps_driver_uninstall, params.p, 0, 180000, &r)) {
        if (params.failed) result_error(t, "Out of memory.");
        sb_free(&params);
        return;
    }
    sb_free(&params);
    doc = script_json(t, &r);
    if (doc) {
        j_bool(doc, "ok", &ok);
        result_json(t, !ok, r.out);
    }
    json_free(doc);
    asb_guest_free(&r);
}

/* ---- install_driver ---- */

typedef struct {
    wchar_t **paths;      /* full host paths */
    int       count, cap;
    UINT64    bytes;
    BOOL      too_many;
} FileList;

static void file_list_free(FileList *list)
{
    int i;
    for (i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
    memset(list, 0, sizeof(*list));
}

/* Collect the files under dir (which the calling thread must be allowed to
   read: call it impersonating). Links and junctions are not followed. */
static void file_list_scan(FileList *list, const wchar_t *dir, int depth)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    if (depth > 6 || swprintf_s(pattern, MAX_PATH, L"%s\\*", dir) < 0) { list->too_many = TRUE; return; }
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0) { list->too_many = TRUE; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { file_list_scan(list, path, depth + 1); continue; }
        if (list->count >= 400) { list->too_many = TRUE; break; }
        if (list->count == list->cap) {
            int cap = list->cap ? list->cap * 2 : 32;
            wchar_t **grown = (wchar_t **)realloc(list->paths, (size_t)cap * sizeof(*grown));
            if (!grown) { list->too_many = TRUE; break; }
            list->paths = grown; list->cap = cap;
        }
        if ((list->paths[list->count] = _wcsdup(path)) == NULL) { list->too_many = TRUE; break; }
        list->count++;
        list->bytes += ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(find, &fd) && !list->too_many);
    FindClose(find);
}

static BOOL ends_with_nocase(const wchar_t *s, const wchar_t *suffix)
{
    size_t n = wcslen(s), m = wcslen(suffix);
    return n >= m && _wcsicmp(s + n - m, suffix) == 0;
}

static void tool_install_driver(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *host_path = j_str(args, "host_path"), *service = j_str(args, "service_name");
    BOOL trust = TRUE, check_only = FALSE, ok = FALSE, legacy = FALSE;
    wchar_t *whost = NULL, root[MAX_PATH] = L"", inf[MAX_PATH] = L"", sys[MAX_PATH] = L"", *leaf;
    char pkg[80] = "", guest_dir[160], guest_file[700], inf_guest[400] = "", sys_guest[400] = "", service_buf[80];
    FileList files;
    DWORD attrs;
    int imp, i, copied = 0, infs = 0, total = 0;
    size_t root_len, k;
    Sb params = { 0 }, js = { 0 };
    AsbGuestResult r;
    JVal *doc;

    memset(&files, 0, sizeof(files));
    if (!resolve_vm(t, args, &vm)) return;
    if (!host_path || !host_path[0]) {
        result_error(t, "Pass host_path: the full path on this PC of the driver's .inf file, of the folder that holds "
                        "the package (.inf, .sys, .cat), or of a single .sys for a legacy (non Plug and Play) driver.");
        return;
    }
    if (service && !plain_name(service, 64)) {
        result_error(t, "service_name may only hold letters, digits, dot, underscore and hyphen.");
        return;
    }
    j_bool(args, "trust_certificate", &trust);
    j_bool(args, "check_only", &check_only);
    if (!guest_ready(t, &vm)) return;

    /* Look at the package with the user's own rights. */
    whost = to_wide(host_path);
    imp = impersonate_plain_user();
    if (imp < 0 || !whost || wcslen(whost) >= MAX_PATH - 2) {
        if (imp > 0) RevertToSelf();
        free(whost);
        result_error(t, "Could not read files with the user's own rights on this PC, or host_path is too long.");
        return;
    }
    for (k = wcslen(whost); k > 3 && (whost[k - 1] == L'\\' || whost[k - 1] == L'/'); k--) whost[k - 1] = L'\0';
    attrs = GetFileAttributesW(whost);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        char why[300];
        describe_win32(GetLastError(), why, sizeof(why));
        RevertToSelf();
        free(whost);
        result_error(t, "Cannot read %s on this PC (%s). host_path must be the full path of the .inf, the package "
                        "folder or a .sys that the user can read.", host_path, why);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        wcscpy_s(root, MAX_PATH, whost);
    } else {
        wcscpy_s(root, MAX_PATH, whost);
        leaf = wcsrchr(root, L'\\');
        if (!leaf) leaf = wcsrchr(root, L'/');
        if (ends_with_nocase(whost, L".inf")) wcscpy_s(inf, MAX_PATH, whost);
        else if (ends_with_nocase(whost, L".sys")) { wcscpy_s(sys, MAX_PATH, whost); legacy = TRUE; }
        if (leaf) *leaf = L'\0';
        if (!leaf || (!inf[0] && !sys[0])) {
            RevertToSelf();
            free(whost);
            result_error(t, "host_path must be an .inf file, a .sys file (legacy driver) or the package folder.");
            return;
        }
    }
    if (legacy) {   /* just the one file */
        files.paths = (wchar_t **)calloc(1, sizeof(*files.paths));
        if (files.paths && (files.paths[0] = _wcsdup(sys)) != NULL) files.count = files.cap = 1;
    } else {
        file_list_scan(&files, root, 0);
    }
    if (imp > 0) RevertToSelf();
    free(whost);
    for (i = 0; !legacy && !inf[0] && i < files.count; i++)   /* a folder: its .inf at the top level */
        if (ends_with_nocase(files.paths[i], L".inf") && !wcschr(files.paths[i] + wcslen(root) + 1, L'\\')) {
            if (!infs++) wcscpy_s(inf, MAX_PATH, files.paths[i]);
        }
    if (!files.count || files.too_many || files.bytes > 1024ull * 1024 * 1024 || (!legacy && !inf[0]) || infs > 1) {
        const char *why = !files.count ? "it has no files"
            : files.too_many || files.bytes > 1024ull * 1024 * 1024
                ? "it holds more than 400 files or 1 GB; point host_path at the folder with only the driver package"
            : infs > 1 ? "the folder has several .inf files; pass the path of the one to install"
            : "the folder has no .inf file at its top level";
        file_list_free(&files);
        result_error(t, "The driver package cannot be used: %s.", why);
        return;
    }

    /* C:\Drivers\<package> in the guest, named after the .inf or .sys. */
    leaf = wcsrchr(legacy ? sys : inf, L'\\');
    leaf = leaf ? leaf + 1 : (legacy ? sys : inf);
    for (k = 0; leaf[k] && leaf[k] != L'.' && k < sizeof(pkg) - 1; k++) {
        wchar_t c = leaf[k];
        pkg[k] = ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')
                 ? (char)c : '_';
    }
    pkg[k] = '\0';
    if (!pkg[0]) strcpy_s(pkg, sizeof(pkg), "driver");
    sprintf_s(guest_dir, sizeof(guest_dir), "C:\\Drivers\\%s", pkg);
    if (legacy && !service) { strcpy_s(service_buf, sizeof(service_buf), pkg); service = service_buf; }

    root_len = wcslen(root);
    total = files.count;
    for (i = 0; i < files.count; i++) {
        char rel[520];
        HANDLE file;
        LARGE_INTEGER size;
        if (!WideCharToMultiByte(CP_UTF8, 0, files.paths[i] + root_len + 1, -1, rel, sizeof(rel), NULL, NULL)) continue;
        sprintf_s(guest_file, sizeof(guest_file), "%s\\%s", guest_dir, rel);
        if (!_wcsicmp(files.paths[i], inf)) strcpy_s(inf_guest, sizeof(inf_guest), guest_file);
        if (!_wcsicmp(files.paths[i], sys)) strcpy_s(sys_guest, sizeof(sys_guest), guest_file);
        imp = impersonate_plain_user();
        if (imp < 0) break;
        file = CreateFileW(files.paths[i], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (imp > 0) RevertToSelf();
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            file_list_free(&files);
            result_error(t, "Cannot read %s from the package folder on this PC; nothing was installed.", rel);
            return;
        }
        asb_guest_write_file(&vm.target, guest_file, file, NULL, (UINT64)size.QuadPart, &r);
        CloseHandle(file);
        if (r.result != ASB_GUEST_OK) { file_list_free(&files); guest_failure(t, &r); return; }
        copied++;
    }
    file_list_free(&files);
    if (i < total || copied < total || (!legacy && !inf_guest[0]) || (legacy && !sys_guest[0])) {
        result_error(t, "The package could not be copied into the guest completely; nothing was installed.");
        return;
    }

    sb_puts(&params, "{\"dir\":");
    sb_json(&params, guest_dir);
    sb_puts(&params, ",\"inf\":");
    if (inf_guest[0]) sb_json(&params, inf_guest); else sb_puts(&params, "null");
    sb_puts(&params, ",\"sys\":");
    if (sys_guest[0]) sb_json(&params, sys_guest); else sb_puts(&params, "null");
    sb_puts(&params, ",\"service\":");
    if (legacy) sb_json(&params, service); else sb_puts(&params, "null");
    sb_printf(&params, ",\"trustCert\":%s,\"checkOnly\":%s}", trust ? "true" : "false", check_only ? "true" : "false");
    if (params.failed || !guest_script(t, &vm, "driver_install", g_ps_driver_install, params.p, 0, 300000, &r)) {
        if (params.failed) result_error(t, "Out of memory.");
        sb_free(&params);
        return;
    }
    sb_free(&params);
    doc = script_json(t, &r);
    if (!doc) { asb_guest_free(&r); return; }
    j_bool(doc, "ok", &ok);
    json_reopen(&js, &r);
    sb_puts(&js, ",\"guestDir\":");
    sb_json(&js, guest_dir);
    sb_printf(&js, ",\"filesCopied\":%d", copied);
    if (!check_only && !vm_has_snapshot(&vm)) {
        sb_puts(&js, ",\"snapshotWarning\":");
        sb_json(&js, g_no_snapshot);
    }
    sb_puts(&js, ",\"next\":");
    sb_json(&js, check_only ? "Nothing was installed (check_only). Fix what warnings lists, then call install_driver again without check_only."
                 : ok ? "Installed. devices lists the devices that use the package with their problem code (0 = working). "
                        "Read the driver's debug output with get_debug_output, test it with run_command, and check "
                        "get_guest_crash if the guest restarts. uninstall_driver removes it again."
                      : "Not installed: warnings and the pnputil / sc output say why. Fix the package on this PC and call install_driver again.");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, !ok, js.p);
    sb_free(&js);
    json_free(doc);
    asb_guest_free(&r);
}

/* ---- get_debug_output ---- */

static void tool_get_debug_output(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *action = j_str(args, "action");
    const JVal *providers = j_get(args, "providers"), *p;
    BOOL enable_filter = FALSE, ok = FALSE;
    int max_bytes = 30000, count = 0;
    Sb params = { 0 }, js = { 0 };
    AsbGuestResult r;
    JVal *doc;

    if (!resolve_vm(t, args, &vm)) return;
    if (!action || (strcmp(action, "start") && strcmp(action, "read") && strcmp(action, "stop") && strcmp(action, "status"))) {
        result_error(t, "action must be \"start\" (begin capturing), \"read\" (the lines since the last read), "
                        "\"stop\" (the last lines, and end the capture) or \"status\".");
        return;
    }
    j_bool(args, "enable_filter", &enable_filter);
    j_int(args, "max_bytes", &max_bytes);
    if (max_bytes < 1000) max_bytes = 1000;
    if (max_bytes > 100000) max_bytes = 100000;
    if (!guest_ready(t, &vm)) return;
    sb_puts(&params, "{\"action\":");
    sb_json(&params, action);
    sb_printf(&params, ",\"enableFilter\":%s,\"max\":%d,\"providers\":[", enable_filter ? "true" : "false", max_bytes);
    for (p = providers && providers->type == J_ARR ? providers->child : NULL; p; p = p->next) {
        size_t i;
        BOOL fine = p->type == J_STR && p->len >= 1 && p->len <= 80 && count < 8;
        for (i = 0; fine && p->str[i]; i++)
            fine = (p->str[i] >= 'a' && p->str[i] <= 'z') || (p->str[i] >= 'A' && p->str[i] <= 'Z') ||
                   (p->str[i] >= '0' && p->str[i] <= '9') || strchr("{}-_. ", p->str[i]) != NULL;
        if (!fine || p->str[0] == '-') {
            sb_free(&params);
            result_error(t, "providers must be up to 8 ETW provider GUIDs (\"{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\", "
                            "e.g. a driver's WPP control GUID) or registered provider names.");
            return;
        }
        sb_puts(&params, count++ ? "," : "");
        sb_json(&params, p->str);
    }
    sb_puts(&params, "]}");
    if (params.failed || !guest_script(t, &vm, "debug_output", g_ps_debug_output, params.p, 0, 180000, &r)) {
        if (params.failed) result_error(t, "Out of memory.");
        sb_free(&params);
        return;
    }
    sb_free(&params);
    doc = script_json(t, &r);
    if (!doc) { asb_guest_free(&r); return; }
    j_bool(doc, "ok", &ok);
    json_reopen(&js, &r);
    sb_puts(&js, ",\"next\":");
    sb_json(&js, !ok ? "See error."
                 : !strcmp(action, "start")
                     ? "Capturing. Load or exercise the driver, then call get_debug_output with action \"read\". Only "
                       "output that passes the debug print filter is captured: DbgPrintEx(DPFLTR_IHVDRIVER_ID, "
                       "DPFLTR_ERROR_LEVEL, ...) always passes; plain DbgPrint / KdPrint (level INFO) passes only after "
                       "start with enable_filter true and a restart. KdPrint also needs a checked (DBG) build. The "
                       "capture ends when the guest restarts: start it again."
                 : !strcmp(action, "read")
                     ? "output holds the lines printed since the last read (time, text). Empty output with a driver that "
                       "prints means the filter dropped it: see debugPrintFilter and the start action's enable_filter."
                 : !strcmp(action, "stop")
                     ? "Capture ended. providerTrace, when present, is an .etl of the ETW/WPP providers: fetch it with "
                       "copy_from_guest and decode it on this PC (tracefmt with the driver's .pdb, or tracerpt)."
                     : "capturing says whether a capture is running.");
    sb_puts(&js, "}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, !ok, js.p);
    sb_free(&js);
    json_free(doc);
    asb_guest_free(&r);
}

/* ---- analyze_dump ---- */

/* The rest of the line that follows `key` at the start of a line. */
static void dump_field(const char *text, const char *key, char *out, size_t cap)
{
    const char *p = text;
    size_t n = strlen(key), len = 0;
    out[0] = '\0';
    while ((p = strstr(p, key)) != NULL) {
        if (p == text || p[-1] == '\n') {
            p += n;
            while (*p == ' ' || *p == '\t' || *p == ':') p++;
            while (p[len] && p[len] != '\r' && p[len] != '\n' && len < cap - 1) len++;
            while (len > 0 && p[len - 1] == ' ') len--;
            memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
        p += n;
    }
}

/* Build the answer from the debugger's "!analyze -v" text. */
static void dump_report(Sb *js, const char *text)
{
    static const char *const keys[][2] = {
        { "PROCESS_NAME", "process" }, { "MODULE_NAME", "module" }, { "IMAGE_NAME", "image" },
        { "SYMBOL_NAME", "symbol" }, { "FAILURE_BUCKET_ID", "failureBucket" }, { "Probably caused by", "probablyCausedBy" },
    };
    const char *banner = strstr(text, "Bugcheck Analysis"), *start = banner ? banner : text, *stack, *line;
    char value[400], name[200] = "";
    size_t i, n;
    int lines = 0, a;

    /* The name is the first line after the banner's box of stars. */
    for (line = banner ? strchr(banner, '\n') : NULL; line && *line; line = strchr(line, '\n')) {
        while (*line == '\r' || *line == '\n') line++;
        if (*line != '*' && *line != ' ' && *line != '\0') {
            for (n = 0; line[n] && line[n] != '\r' && line[n] != '\n' && n < sizeof(name) - 1; n++) name[n] = line[n];
            name[n] = '\0';
            start = line;   /* the analysis text starts here, after the banner */
            break;
        }
    }
    dump_field(text, "BUGCHECK_CODE", value, sizeof(value));
    sb_puts(js, ",\"bugcheck\":{\"code\":");
    if (value[0]) { char code[80]; sprintf_s(code, sizeof(code), "0x%.60s", value); sb_json(js, code); } else sb_puts(js, "null");
    sb_puts(js, ",\"name\":");
    sb_json(js, name);
    sb_puts(js, ",\"args\":[");
    for (a = 1; a <= 4; a++) {
        char key[20];
        sprintf_s(key, sizeof(key), "BUGCHECK_P%d", a);
        dump_field(text, key, value, sizeof(value));
        sb_puts(js, a > 1 ? "," : "");
        sb_json(js, value);
    }
    sb_puts(js, "]}");
    for (i = 0; i < ARRAYSIZE(keys); i++) {
        dump_field(text, keys[i][0], value, sizeof(value));
        sb_printf(js, ",\"%s\":", keys[i][1]);
        if (value[0]) sb_json(js, value); else sb_puts(js, "null");
    }
    sb_puts(js, ",\"stack\":[");
    stack = strstr(text, "\nSTACK_TEXT:");
    for (line = stack ? strchr(stack + 1, '\n') : NULL; line && lines < 40; line = strchr(line, '\n')) {
        line++;
        if (*line == '\r') line++;
        for (n = 0; line[n] && line[n] != '\r' && line[n] != '\n'; n++) {}
        if (!n) break;
        sb_puts(js, lines++ ? "," : "");
        sb_json_n(js, line, n);
    }
    sb_puts(js, "],\"analysis\":");
    n = strlen(start);
    sb_json_n(js, start, n > 14000 ? 14000 : n);
    sb_printf(js, ",\"analysisTruncated\":%s", n > 14000 ? "true" : "false");
}

static const char g_no_debugger[] =
    "There is no debugger (cdb.exe or kd.exe) on this PC or in the guest, so the dump was not analysed. Install "
    "one on this PC, then call analyze_dump again: either the Debugging Tools for Windows from the Windows SDK "
    "(winget install --id Microsoft.WindowsSDK.10.0.26100, or the SDK installer with the feature "
    "OptionId.WindowsDesktopDebuggers; cdb.exe lands in C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64), "
    "or WinDbg (winget install --id Microsoft.WinDbg). Meanwhile get_guest_crash gives the stop code and its "
    "parameters, and copy_from_guest fetches the dump.";

static void tool_analyze_dump(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *dump_arg = j_str(args, "dump_path"), *host_dump = j_str(args, "host_dump_path");
    const char *symbol_path = j_str(args, "symbol_path");
    wchar_t debugger[MAX_PATH] = L"", host_file[MAX_PATH] = L"", cache[MAX_PATH] = L"", *wsym = NULL, *command = NULL, *sympath = NULL;
    char guest_dump[400] = "", guest_debugger[400] = "", why[300];
    int timeout = 300, max_mb = 1024, imp;
    UINT64 bytes = 0;
    BOOL on_host, timed_out = FALSE;
    DWORD exit_code = 0;
    Sb params = { 0 }, js = { 0 }, text = { 0 };
    AsbGuestResult r;
    JVal *doc = NULL;
    size_t cch;

    if (!resolve_vm(t, args, &vm)) return;
    j_int(args, "timeout_seconds", &timeout);
    if (timeout < 30) timeout = 30;
    if (timeout > 600) timeout = 600;
    j_int(args, "max_mb", &max_mb);
    if (max_mb < 1) max_mb = 1;
    if (max_mb > 4096) max_mb = 4096;
    if ((dump_arg && strchr(dump_arg, '"')) || (symbol_path && strlen(symbol_path) > 1000)) {
        result_error(t, "dump_path must not contain quotes, and symbol_path is limited to 1000 characters.");
        return;
    }
    on_host = find_host_debugger(debugger, MAX_PATH);

    if (host_dump) {   /* a dump that is already on this PC */
        wchar_t *w = to_wide(host_dump);
        if (!on_host) { free(w); result_error(t, "%s", g_no_debugger); return; }
        imp = impersonate_plain_user();
        if (imp < 0 || !w || wcslen(w) >= MAX_PATH || wcschr(w, L'"') || !file_exists(w)) {
            if (imp > 0) RevertToSelf();
            free(w);
            result_error(t, "host_dump_path must be the full path of a dump file on this PC that the user can read.");
            return;
        }
        if (imp > 0) RevertToSelf();
        wcscpy_s(host_file, MAX_PATH, w);
        free(w);
    } else {
        const char *s;
        BOOL found = FALSE;
        if (!guest_ready(t, &vm)) return;
        sb_puts(&params, "{\"path\":");
        if (dump_arg && dump_arg[0]) sb_json(&params, dump_arg); else sb_puts(&params, "null");
        sb_puts(&params, "}");
        if (params.failed || !guest_script(t, &vm, "find_dump", g_ps_find_dump, params.p, 0, 60000, &r)) {
            if (params.failed) result_error(t, "Out of memory.");
            sb_free(&params);
            return;
        }
        sb_free(&params);
        doc = script_json(t, &r);
        asb_guest_free(&r);
        if (!doc) return;
        j_bool(doc, "found", &found);
        if ((s = j_str(doc, "path")) != NULL) strncpy_s(guest_dump, sizeof(guest_dump), s, _TRUNCATE);
        if ((s = j_str(doc, "guestDebugger")) != NULL) strncpy_s(guest_debugger, sizeof(guest_debugger), s, _TRUNCATE);
        {
            const JVal *b = j_get(doc, "bytes");
            if (b && b->type == J_NUM && b->num > 0) bytes = (UINT64)b->num;
        }
        json_free(doc);
        if (!found || !guest_dump[0] || strchr(guest_dump, '"')) {
            result_error(t, dump_arg ? "There is no such dump file in the guest. get_guest_crash lists the dump files."
                                     : "The guest has no crash dump (no file in C:\\Windows\\Minidump and no "
                                       "C:\\Windows\\MEMORY.DMP). get_guest_crash shows whether Windows recorded a "
                                       "bugcheck; a live kernel report can be analysed by passing its path as dump_path.");
            return;
        }
        if (!on_host && !guest_debugger[0]) { result_error(t, "%s", g_no_debugger); return; }
        if (on_host) {
            if (bytes > (UINT64)max_mb * 1024 * 1024) {
                result_error(t, "The dump %s is %llu MB, more than max_mb (%d). Pass a larger max_mb, or analyse a "
                                "minidump instead (get_guest_crash lists them).", guest_dump,
                             (unsigned long long)(bytes / (1024 * 1024)) + 1, max_mb);
                return;
            }
            if (!pull_guest_file(t, &vm, guest_dump, NULL, max_mb, host_file, &bytes)) return;
        }
    }

    if (on_host) {
        /* Symbols: the caller's folder first, then Microsoft's server with a cache the user owns. */
        imp = impersonate_plain_user();
        if (imp >= 0 && GetEnvironmentVariableW(L"LOCALAPPDATA", cache, MAX_PATH - 40)) {
            wcscat_s(cache, MAX_PATH, L"\\AppSandbox\\symbols");
            make_dirs(cache);
        }
        if (imp > 0) RevertToSelf();
        wsym = symbol_path && symbol_path[0] ? to_wide(symbol_path) : NULL;
        cch = (wsym ? wcslen(wsym) : 0) + wcslen(cache) + 100;
        sympath = (wchar_t *)malloc(cch * sizeof(wchar_t));
        command = (wchar_t *)malloc((2 * MAX_PATH + 80) * sizeof(wchar_t));
        if (imp < 0 || !cache[0] || !sympath || !command) {
            free(wsym); free(sympath); free(command);
            result_error(t, "Could not prepare the symbol cache with the user's own rights on this PC.");
            return;
        }
        swprintf_s(sympath, cch, L"%s%ssrv*%s*https://msdl.microsoft.com/download/symbols", wsym ? wsym : L"",
                   wsym ? L";" : L"", cache);
        swprintf_s(command, 2 * MAX_PATH + 80, L"\"%s\" -z \"%s\" -c \"!analyze -v; q\"", debugger, host_file);
        if (!run_plain_process(command, L"_NT_SYMBOL_PATH", sympath, (DWORD)timeout * 1000, 600000, &text,
                               &exit_code, &timed_out, why, sizeof(why))) {
            free(wsym); free(sympath); free(command);
            sb_free(&text);
            result_error(t, "The debugger on this PC could not be started (%s). If it is the one inside the WinDbg "
                            "app, install the Debugging Tools for Windows instead (Windows SDK feature "
                            "OptionId.WindowsDesktopDebuggers): its cdb.exe can be started directly.", why);
            return;
        }
        free(wsym); free(sympath); free(command);
    } else {
        /* No debugger here, but the guest has one: run it there. */
        char guest_command[1200];
        sprintf_s(guest_command, sizeof(guest_command),
                  "\"%s\" -z \"%s\" -y \"srv*C:\\Windows\\Temp\\AppSandbox\\symbols*https://msdl.microsoft.com/download/symbols\" "
                  "-c \"!analyze -v; q\"", guest_debugger, guest_dump);
        asb_guest_exec(&vm.target, guest_command, NULL, 0, (DWORD)timeout * 1000, 600000, &r);
        if (r.result != ASB_GUEST_OK) { guest_failure(t, &r); asb_guest_free(&r); return; }
        sb_put(&text, r.out, r.out_len);
        timed_out = r.timed_out;
        exit_code = r.exit_code;
        asb_guest_free(&r);
    }
    if (text.failed || !text.p || !strstr(text.p, "BUGCHECK_CODE")) {
        size_t n = text.p ? strlen(text.p) : 0;
        result_error(t, "The debugger ran%s but produced no bugcheck analysis (exit code %ld). This may not be a "
                        "kernel crash dump, the symbols could not be downloaded, or it needs more time "
                        "(timeout_seconds, at most 600). Its last output: %.700s", timed_out ? " into the time limit" : "",
                     (long)(int)exit_code, n ? text.p + (n > 700 ? n - 700 : 0) : "nothing");
        sb_free(&text);
        return;
    }
    sb_puts(&js, "{\"dump\":{\"guestPath\":");
    if (guest_dump[0]) sb_json(&js, guest_dump); else sb_puts(&js, "null");
    sb_puts(&js, ",\"hostPath\":");
    if (host_file[0]) sb_json_w(&js, host_file); else sb_puts(&js, "null");
    sb_printf(&js, ",\"bytes\":%llu},\"debugger\":", (unsigned long long)bytes);
    if (on_host) sb_json_w(&js, debugger); else sb_json(&js, guest_debugger);
    sb_printf(&js, ",\"ranOn\":\"%s\",\"timedOut\":%s", on_host ? "host" : "guest", timed_out ? "true" : "false");
    dump_report(&js, text.p);
    sb_puts(&js, ",\"next\":\"module / image / symbol name the code that was running when the guest crashed; stack is "
                 "the call stack, innermost first. Frames without names from your driver mean its symbols were not "
                 "found: pass symbol_path, the folder on this PC that holds its .pdb.\"}");
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, FALSE, js.p);
    sb_free(&js);
    sb_free(&text);
}

/* ---- send_input ---- */

typedef struct {
    InputPacket *p;
    int n, cap;
    BOOL failed;
} PacketList;

static void packet_add(PacketList *l, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        InputPacket *grown = cap <= 20000 ? (InputPacket *)realloc(l->p, (size_t)cap * sizeof(*grown)) : NULL;
        if (!grown) { l->failed = TRUE; return; }
        l->p = grown; l->cap = cap;
    }
    l->p[l->n].magic = INPUT_MAGIC;
    l->p[l->n].type = type;
    l->p[l->n].param1 = p1; l->p[l->n].param2 = p2; l->p[l->n].param3 = p3;
    l->n++;
}

/* INPUT_KEY: virtual key, scan code, flags (1 = extended key, 2 = key up). */
static void packet_key(PacketList *l, UINT vk, BOOL down)
{
    UINT flags = down ? 0 : INPUT_KEY_UP;
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE: case VK_NUMLOCK: case VK_SNAPSHOT:
        flags |= INPUT_KEY_EXTENDED;
        break;
    }
    packet_add(l, INPUT_KEY, vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), flags);
}

/* The virtual key a key name stands for, 0 when it is not one. */
static UINT key_from_name(const char *name)
{
    static const struct { const char *name; UINT vk; } keys[] = {
        { "enter", VK_RETURN }, { "return", VK_RETURN }, { "tab", VK_TAB }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
        { "space", VK_SPACE }, { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "del", VK_DELETE },
        { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
        { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }, { "win", VK_LWIN },
        { "windows", VK_LWIN }, { "ctrl", VK_CONTROL }, { "control", VK_CONTROL }, { "alt", VK_MENU },
        { "shift", VK_SHIFT }, { "menu", VK_APPS }, { "printscreen", VK_SNAPSHOT }, { "capslock", VK_CAPITAL },
        { "plus", VK_OEM_PLUS }, { "minus", VK_OEM_MINUS },
    };
    size_t i;
    for (i = 0; i < ARRAYSIZE(keys); i++)
        if (!_stricmp(name, keys[i].name)) return keys[i].vk;
    if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 24 && strlen(name) <= 3) return VK_F1 + (UINT)n - 1;
    }
    if (strlen(name) == 1) {
        SHORT scan = VkKeyScanW((wchar_t)(unsigned char)name[0]);
        if (scan != -1) return (UINT)(scan & 0xFF);
    }
    return 0;
}

/* "ctrl+shift+esc": every key down in order, then up in reverse. */
static BOOL packet_chord(PacketList *l, const char *chord)
{
    UINT vks[6];
    int n = 0, i;
    char token[24];
    const char *p = chord;
    while (*p) {
        size_t len = 0;
        while (p[len] && p[len] != '+') len++;
        if (!len && p[0] == '+') len = 1;           /* a literal "+" key */
        if (len >= sizeof(token) || n >= (int)ARRAYSIZE(vks)) return FALSE;
        memcpy(token, p, len); token[len] = '\0';
        if ((vks[n] = key_from_name(len == 1 && token[0] == '+' ? "plus" : token)) == 0) return FALSE;
        n++;
        p += len;
        if (*p == '+') p++;
    }
    if (!n) return FALSE;
    for (i = 0; i < n; i++) packet_key(l, vks[i], TRUE);
    for (i = n - 1; i >= 0; i--) packet_key(l, vks[i], FALSE);
    return TRUE;
}

/* The window that has the keyboard focus in the guest right now, as a JSON
   object {title, process}, when the guest can tell (Windows guest, agent
   online and able to run a script in the desktop session). Best effort. */
static void focused_window(VmRef *vm, Sb *out)
{
    AsbGuestResult r;
    ToolCall quiet;
    JVal *doc;

    if (_stricmp(vm->os, "Windows") != 0 || !vm->agent_online) return;
    fill_target(vm);
    asb_guest_ping(&vm->target, &r);
    if (r.result != ASB_GUEST_OK || !strstr(r.message, "desktop")) return;
    memset(&quiet, 0, sizeof(quiet));
    if (guest_script(&quiet, vm, "foreground", g_ps_foreground, "{}", ASB_EXEC_DESKTOP, 30000, &r)) {
        doc = r.out_len ? json_parse(r.out, r.out_len) : NULL;
        if (doc && doc->type == J_OBJ && r.out_len < 1000) sb_put(out, r.out, r.out_len);
        json_free(doc);
        asb_guest_free(&r);
    }
    sb_free(&quiet.out);
}

typedef struct { UINT64 id; const InputPacket *p; int count, sent; } InputJob;

static void input_window_fn(void *param)
{
    InputJob *job = (InputJob *)param;
    job->sent = asb_api_display_send_packets(job->id, job->p, job->count);
}

static void tool_send_input(ToolCall *t, const JVal *args)
{
    VmRef vm;
    const char *text = j_str(args, "text"), *keys = j_str(args, "keys"), *button = j_str(args, "button");
    int x = 0, y = 0, scroll = 0, skipped = 0, sent = 0, stalls = 0;
    BOOL has_x = j_int(args, "x", &x), has_y = j_int(args, "y", &y), has_scroll = j_int(args, "scroll", &scroll);
    BOOL twice = FALSE, via_window = FALSE;
    PacketList list = { 0 };
    Sb js = { 0 }, focus = { 0 };

    if (!resolve_vm(t, args, &vm)) return;
    if (has_x != has_y || (!has_x && !text && !keys && !has_scroll) || (text && strlen(text) > 2000)) {
        result_error(t, "Pass at least one of: text (up to 2000 characters), keys (e.g. \"enter\", \"ctrl+shift+esc\", "
                        "several separated by spaces), x and y together (guest pixels, as in a full-size screenshot) "
                        "with an optional button, or scroll.");
        return;
    }
    if (!vm.running || !_stricmp(vm.os, "Other")) {
        result_error(t, !vm.running ? "The VM is not running. Start it with start_vm first."
                                    : "Input needs App Sandbox's guest components, which a manual-install VM does not have.");
        return;
    }
    j_bool(args, "double_click", &twice);
    if (has_x) {
        UINT32 which = INPUT_BTN_LEFT;
        BOOL click = TRUE;
        if (x < 0 || y < 0 || x > 16384 || y > 16384) { result_error(t, "x and y are guest pixels, from 0."); return; }
        if (button && !_stricmp(button, "right")) which = INPUT_BTN_RIGHT;
        else if (button && !_stricmp(button, "middle")) which = INPUT_BTN_MIDDLE;
        else if (button && !_stricmp(button, "none")) click = FALSE;
        else if (button && button[0] && _stricmp(button, "left")) {
            result_error(t, "button must be \"left\" (default), \"right\", \"middle\" or \"none\" (move only).");
            return;
        }
        packet_add(&list, INPUT_MOUSE_MOVE, (UINT32)x, (UINT32)y, 0);
        if (click) {
            int times = twice ? 2 : 1;
            while (times--) {
                packet_add(&list, INPUT_MOUSE_BUTTON, which, 1, 0);
                packet_add(&list, INPUT_MOUSE_BUTTON, which, 0, 0);
            }
        }
    }
    if (has_scroll) {
        if (scroll < -100 || scroll > 100) { free(list.p); result_error(t, "scroll is in wheel notches, -100 to 100."); return; }
        packet_add(&list, INPUT_MOUSE_WHEEL, (UINT32)(scroll * 120), 0, 0);
    }
    if (text) {
        wchar_t *wide = to_wide(text), *c;
        for (c = wide; c && *c; c++) {
            SHORT scan = *c == L'\n' ? (SHORT)VK_RETURN : *c == L'\t' ? (SHORT)VK_TAB : VkKeyScanW(*c);
            UINT mods = (UINT)(scan >> 8) & 0xFF;
            if (*c == L'\r') continue;
            if (scan == -1 || (mods & ~7u)) { skipped++; continue; }
            if (mods & 1) packet_key(&list, VK_SHIFT, TRUE);
            if (mods & 2) packet_key(&list, VK_CONTROL, TRUE);
            if (mods & 4) packet_key(&list, VK_MENU, TRUE);
            packet_key(&list, (UINT)(scan & 0xFF), TRUE);
            packet_key(&list, (UINT)(scan & 0xFF), FALSE);
            if (mods & 4) packet_key(&list, VK_MENU, FALSE);
            if (mods & 2) packet_key(&list, VK_CONTROL, FALSE);
            if (mods & 1) packet_key(&list, VK_SHIFT, FALSE);
        }
        free(wide);
    }
    if (keys) {
        char chord[64];
        const char *p = keys;
        while (*p) {
            size_t len = 0;
            while (*p == ' ') p++;
            while (p[len] && p[len] != ' ') len++;
            if (!len) break;
            if (len >= sizeof(chord)) { list.failed = TRUE; break; }
            memcpy(chord, p, len); chord[len] = '\0';
            if (!packet_chord(&list, chord)) {
                free(list.p);
                result_error(t, "\"%s\" is not a key. Use names such as enter, tab, esc, space, backspace, delete, "
                                "home, end, pageup, pagedown, up, down, left, right, f1 to f24, win, or a single "
                                "character, joined with + for a chord (ctrl+c, alt+f4, win+r).", chord);
                return;
            }
            p += len;
        }
    }
    if (list.failed || !list.n) {
        free(list.p);
        result_error(t, list.failed ? "That is too much input for one call; send it in parts." : "Nothing to send.");
        return;
    }
    focused_window(&vm, &focus);

    if (vm.display_open) {
        /* The VM's window owns the input connection: send through it, a few
           packets at a time (its sends never wait). */
        InputJob job;
        job.id = vm.num_id;
        while (sent < list.n && stalls < 100 && !asb_api_stopping()) {
            job.p = list.p + sent;
            job.count = list.n - sent > 6 ? 6 : list.n - sent;
            job.sent = -1;
            if (!asb_api_run(input_window_fn, &job) || job.sent < 0) break;
            via_window = TRUE;
            sent += job.sent;
            if (job.sent < job.count) stalls++;
            Sleep(job.sent < job.count ? 40 : 15);
        }
    }
    if (!via_window) {
        AsbGuestResult r;
        fill_target(&vm);
        asb_guest_send_input(&vm.target, list.p, list.n, 6, &r);
        if (r.result != ASB_GUEST_OK) { free(list.p); guest_failure(t, &r); return; }
        sent = list.n;
    }
    sb_printf(&js, "{\"sent\":%d,\"of\":%d,\"via\":\"%s\",\"skippedCharacters\":%d", sent, list.n,
              via_window ? "the VM's window" : "a direct connection", skipped);
    /* Keys go to whatever has the focus in the guest: say what that was. */
    if (focus.p) { sb_puts(&js, ",\"focusedWindow\":"); sb_puts(&js, focus.p); }
    sb_puts(&js, ",\"next\":");
    sb_free(&focus);
    sb_json(&js, sent < list.n ? "Not everything was sent (the connection was busy). Call screenshot to see where it stopped."
                 : skipped ? "Sent, except the characters this PC's keyboard layout has no key for. Text is typed as key "
                             "presses, so the guest needs the same keyboard layout as this PC. Call screenshot to check."
                           : "Sent. Call screenshot to see the result.");
    sb_puts(&js, "}");
    free(list.p);
    if (js.failed) result_error(t, "Out of memory."); else result_json(t, sent < list.n, js.p);
    sb_free(&js);
}

/* ---- screenshot through the guest ---- */

/* When the display channel gives no picture (nothing on the screen changed
   since a performance-mode window closed, or that window is open), copy the
   screen inside the guest: a script in the signed-in user's desktop session.
   TRUE when the tool result is set (picture or a specific error). */
static BOOL guest_screenshot(ToolCall *t, VmRef *vm, int max_width)
{
    AsbGuestResult r, file;
    char params[300], shot[160];
    JVal *doc;
    int w = 0, h = 0, sw = 0, sh = 0;

    if (_stricmp(vm->os, "Windows") != 0 || !vm->agent_online) return FALSE;
    fill_target(vm);
    asb_guest_ping(&vm->target, &r);
    if (r.result != ASB_GUEST_OK || !strstr(r.message, "desktop")) return FALSE;   /* older agent */
    sprintf_s(shot, sizeof(shot), "C:\\Windows\\Temp\\AppSandbox\\shot-%lu.png", (unsigned long)GetTickCount());
    sprintf_s(params, sizeof(params), "{\"out\":\"C:\\\\Windows\\\\Temp\\\\AppSandbox\\\\%s\",\"maxWidth\":%d}",
              strrchr(shot, '\\') + 1, max_width);
    {
        ToolCall quiet;
        BOOL ran;
        memset(&quiet, 0, sizeof(quiet));
        ran = guest_script(&quiet, vm, "screenshot", g_ps_screenshot, params, ASB_EXEC_DESKTOP, 60000, &r);
        sb_free(&quiet.out);
        if (!ran) return FALSE;
    }
    doc = r.out_len ? json_parse(r.out, r.out_len) : NULL;
    if (!doc || !j_int(doc, "width", &w) || !j_int(doc, "height", &h)) {
        json_free(doc);
        result_error(t, "The guest's screen could not be copied: %.500s. Nobody may be signed in yet (the lock or "
                        "sign-in screen cannot be copied this way); open_display shows the VM to the user.",
                     r.err_len ? r.err : (r.out_len ? r.out : "the capture script printed nothing"));
        asb_guest_free(&r);
        return TRUE;
    }
    j_int(doc, "screenWidth", &sw);
    j_int(doc, "screenHeight", &sh);
    json_free(doc);
    asb_guest_free(&r);
    asb_guest_read_file(&vm->target, shot, NULL, 32u * 1024 * 1024, &file);
    {
        char del[220];
        sprintf_s(del, sizeof(del), "cmd.exe /c del /q \"%s\"", shot);
        asb_guest_exec(&vm->target, del, NULL, 0, 15000, 1000, &r);
        asb_guest_free(&r);
    }
    if (file.result != ASB_GUEST_OK || file.out_len < 8 || memcmp(file.out, "\x89PNG", 4) != 0) {
        asb_guest_free(&file);
        result_error(t, "The guest made a screenshot but it could not be read back. Try again.");
        return TRUE;
    }
    t->is_error = FALSE;
    t->out.len = 0;
    sb_puts(&t->out, "{\"content\":[{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
    sb_base64(&t->out, (const BYTE *)file.out, file.out_len);
    sb_puts(&t->out, "\"},{\"type\":\"text\",\"text\":");
    {
        char note[300];
        sprintf_s(note, sizeof(note), "Screenshot of \"%s\" taken inside the guest: %dx%d pixels%s (guest resolution %dx%d).",
                  vm->name, w, h, w != sw ? ", scaled down" : "", sw, sh);
        sb_json(&t->out, note);
    }
    sb_puts(&t->out, "}],\"isError\":false");
    asb_guest_free(&file);
    return TRUE;
}

/* ---- screenshot ---- */

typedef struct {
    UINT64      id;
    BYTE       *pixels;        /* from the open window, when it has one */
    UINT        w, h, stride;
    int         why;
    AsbDisplay *display;       /* otherwise: a reader of our own */
} ShotGrab;

static void shot_window_fn(void *param)
{
    ShotGrab *g = (ShotGrab *)param;
    g->pixels = asb_api_display_copy_frame(g->id, &g->w, &g->h, &g->stride, &g->why);
}

static void shot_connect_fn(void *param)
{
    ShotGrab *g = (ShotGrab *)param;
    VmInstance *v = asb_find_vm_by_id(g->id);
    g->display = v ? asb_display_connect((AsbVm)v) : NULL;
}

/* BGRA -> 24-bit BGR, shrunk by a whole factor (box average) so the width is
   at most max_w. Returns a malloc'd buffer. */
static BYTE *shrink_bgr(const BYTE *bgra, UINT w, UINT h, UINT stride, UINT max_w,
                        UINT *out_w, UINT *out_h, UINT *out_stride)
{
    UINT f = (w + max_w - 1) / max_w, ow, oh, os, x, y, dx, dy;
    BYTE *out;
    if (f < 1) f = 1;
    ow = w / f; oh = h / f;
    if (!ow || !oh) return NULL;
    os = (ow * 3 + 3) & ~3u;
    out = (BYTE *)malloc((size_t)os * oh);
    if (!out) return NULL;
    for (y = 0; y < oh; y++) {
        BYTE *row = out + (size_t)y * os;
        for (x = 0; x < ow; x++) {
            UINT b = 0, g = 0, r = 0;
            for (dy = 0; dy < f; dy++) {
                const BYTE *src = bgra + (size_t)(y * f + dy) * stride + (size_t)x * f * 4;
                for (dx = 0; dx < f; dx++, src += 4) { b += src[0]; g += src[1]; r += src[2]; }
            }
            row[x * 3] = (BYTE)(b / (f * f));
            row[x * 3 + 1] = (BYTE)(g / (f * f));
            row[x * 3 + 2] = (BYTE)(r / (f * f));
        }
    }
    *out_w = ow; *out_h = oh; *out_stride = os;
    return out;
}

/* Encode 24-bit BGR rows as PNG with the Windows Imaging Component. */
static BYTE *png_encode(BYTE *bgr, UINT w, UINT h, UINT stride, size_t *out_len)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    IStream *stream = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    HGLOBAL memory = NULL;
    STATSTG stat;
    BYTE *png = NULL;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IWICImagingFactory, (void **)&factory)) &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, w, h)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &format)) &&
        IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, h, stride, stride * h, bgr)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) &&
        SUCCEEDED(IWICBitmapEncoder_Commit(encoder)) &&
        SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) &&
        SUCCEEDED(GetHGlobalFromStream(stream, &memory)) && stat.cbSize.HighPart == 0) {
        const void *bytes = GlobalLock(memory);
        if (bytes) {
            png = (BYTE *)malloc(stat.cbSize.LowPart ? stat.cbSize.LowPart : 1);
            if (png) { memcpy(png, bytes, stat.cbSize.LowPart); *out_len = stat.cbSize.LowPart; }
            GlobalUnlock(memory);
        }
    }
    if (props) IPropertyBag2_Release(props);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    return png;
}

static void tool_screenshot(ToolCall *t, const JVal *args)
{
    VmRef vm;
    ShotGrab grab = { 0 };
    int max_width = 1280;
    BYTE *bgr, *png;
    UINT ow = 0, oh = 0, os = 0;
    size_t png_len = 0;
    const char *source = "the open VM window";

    if (!resolve_vm(t, args, &vm)) return;
    j_int(args, "max_width", &max_width);
    if (max_width < 320) max_width = 320;
    if (max_width > 3840) max_width = 3840;
    if (!vm.running) { result_error(t, "The VM is not running. Start it with start_vm first."); return; }
    if (!_stricmp(vm.os, "Other")) {
        result_error(t, "screenshot needs App Sandbox's display driver in the guest, which a manual-install VM "
                        "(OS type Other) does not have. Ask the user to look at the VM's console window.");
        return;
    }
    grab.id = vm.num_id;
    EnterCriticalSection(&g_shot_cs);
    if (vm.display_open) {
        /* The window owns the frame channel (one reader at a time): use its picture. */
        int tries;
        for (tries = 0; tries < 20 && !grab.pixels && grab.why == 0; tries++) {
            if (tries) Sleep(250);
            if (!asb_api_run(shot_window_fn, &grab)) break;
        }
    } else {
        ULONGLONG deadline = GetTickCount64() + 3500;
        UINT stride = 0;
        source = "the guest display";
        if (asb_api_run(shot_connect_fn, &grab) && grab.display) {
            while (GetTickCount64() < deadline && !asb_api_stopping()) {
                if (asb_display_screenshot(grab.display, NULL, &grab.w, &grab.h, &stride) && grab.w && grab.h) {
                    /* The size can change between the two calls: leave room. */
                    BYTE *buf = (BYTE *)malloc((size_t)stride * grab.h + 64);
                    UINT w2 = 0, h2 = 0, s2 = 0;
                    if (buf && asb_display_screenshot(grab.display, NULL, &w2, &h2, &s2) && w2 == grab.w &&
                        h2 == grab.h && asb_display_screenshot(grab.display, buf, &w2, &h2, &s2) &&
                        w2 == grab.w && h2 == grab.h) {
                        grab.pixels = buf;
                        grab.stride = s2;
                        break;
                    }
                    free(buf);
                }
                Sleep(150);
            }
            asb_display_disconnect(grab.display);
        }
    }
    LeaveCriticalSection(&g_shot_cs);

    /* The driver only sends a picture when the screen changes (or when it
       still has one from before): with nothing moving after a
       performance-mode window, and while such a window is open, there is
       none to get. Then the guest copies its own screen. */
    if (!grab.pixels && guest_screenshot(t, &vm, max_width)) return;
    if (!grab.pixels) {
        if (grab.why == 1)
            result_error(t, "The VM's window is open in performance mode (GPU video), which keeps no picture to "
                            "copy, and the guest could not copy its own screen (its agent is offline or older than "
                            "this app: shut the VM down and start it again to update the guest components). "
                            "Otherwise ask the user to switch the window to quality mode.");
        else if (vm.display_open)
            result_error(t, "The VM's window has not received a picture yet. Try again in a few seconds.");
        else
            result_error(t, "No picture arrived from the guest display, and the guest could not copy its own "
                            "screen (its agent is offline or older than this app: shut the VM down and start it "
                            "again to update the guest components). The display driver may not be up yet (the VM "
                            "is still booting or installing: call wait_for_vm); get_vm shows displayDriverReady.");
        return;
    }
    bgr = shrink_bgr(grab.pixels, grab.w, grab.h, grab.stride, (UINT)max_width, &ow, &oh, &os);
    free(grab.pixels);
    png = bgr ? png_encode(bgr, ow, oh, os, &png_len) : NULL;
    free(bgr);
    if (!png) { result_error(t, "The picture could not be encoded as PNG."); return; }

    t->is_error = FALSE;
    t->out.len = 0;
    sb_puts(&t->out, "{\"content\":[{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
    sb_base64(&t->out, png, png_len);
    sb_puts(&t->out, "\"},{\"type\":\"text\",\"text\":");
    {
        char note[300];
        sprintf_s(note, sizeof(note), "Screenshot of \"%s\" from %s: %ux%u pixels%s (guest resolution %ux%u).",
                  vm.name, source, ow, oh, ow != grab.w ? ", scaled down" : "", grab.w, grab.h);
        sb_json(&t->out, note);
    }
    sb_puts(&t->out, "}],\"isError\":false");
    free(png);
}

/* ---------------------------------------------------------------- tool table */

typedef struct {
    const char *name, *title, *description, *schema;
    BOOL read_only, destructive, idempotent, open_world;
    BOOL changes;     /* hidden and refused in read-only mode */
    void (*run)(ToolCall *, const JVal *);
} Tool;

#define VM_PROP "\"vm\":{\"type\":\"string\",\"description\":\"The VM's name or id (from list_vms).\"}"
#define VM_ONLY "{\"type\":\"object\",\"properties\":{" VM_PROP "},\"required\":[\"vm\"],\"additionalProperties\":false}"
#define NO_ARGS "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"

static const Tool g_tools[] = {
    { "list_vms", "List virtual machines",
      "List every virtual machine with its id, name, OS, state (stopped, building, installing, booting, online, "
      "stopping), whether its guest agent is online, RAM, CPU cores, GPU and SSH port. Call this first.",
      NO_ARGS, TRUE, FALSE, TRUE, FALSE, FALSE, tool_list_vms },
    { "get_vm", "Get one VM's status",
      "Full status of one VM: state, install progress, agentOnline, IP address (NAT), GPU, SSH port, whether its "
      "display driver is ready, whether its guest components are older than this app, test mode, and the last "
      "guest crash (bugcheck) the guest reported, if any.",
      VM_ONLY, TRUE, FALSE, TRUE, FALSE, FALSE, tool_get_vm },
    { "get_host", "Get this PC's capacity",
      "This PC as a VM host: CPU cores, RAM, free disk space, GPUs that can be shared with VMs, the templates "
      "new VMs can be created from, the app version, and whether this MCP server is read-only. Use it before "
      "create_vm to pick sizes.",
      NO_ARGS, TRUE, FALSE, TRUE, FALSE, FALSE, tool_get_host },
    { "create_vm", "Create a virtual machine",
      "Create a new VM and start its unattended OS installation (10 to 40 minutes; follow with wait_for_vm). Give "
      "an installer ISO on this PC (image_path) or a template name (from get_host). For driver work on Windows "
      "set test_mode true: Secure Boot off and test signing on, so test-signed drivers load. Returns the new "
      "VM's id.",
      "{\"type\":\"object\",\"properties\":{"
      "\"name\":{\"type\":\"string\",\"description\":\"VM name, also the guest's host name: letters, digits, hyphens; at most 15 characters.\"},"
      "\"os_type\":{\"type\":\"string\",\"enum\":[\"Windows\"],\"description\":\"Windows 11.\"},"
      "\"image_path\":{\"type\":\"string\",\"description\":\"Full path on this PC of the installer ISO. Not needed with template.\"},"
      "\"template\":{\"type\":\"string\",\"description\":\"Name of a template from get_host; much faster than an ISO.\"},"
      "\"admin_user\":{\"type\":\"string\",\"description\":\"Name of the administrator account created in the guest.\"},"
      "\"admin_password\":{\"type\":\"string\",\"description\":\"Its password.\"},"
      "\"ram_mb\":{\"type\":\"integer\",\"description\":\"Memory in MB (even number, at least 512). Default 4096.\"},"
      "\"disk_gb\":{\"type\":\"integer\",\"description\":\"Disk size in GB. Default 64.\"},"
      "\"cpu_cores\":{\"type\":\"integer\",\"description\":\"Virtual processors. Default 4.\"},"
      "\"gpu\":{\"type\":\"boolean\",\"description\":\"Share this PC's GPU with the VM (GPU-PV). Default true.\"},"
      "\"network\":{\"type\":\"string\",\"enum\":[\"nat\",\"none\",\"external\",\"internal\"],\"description\":\"Default nat (internet access through this PC).\"},"
      "\"ssh\":{\"type\":\"boolean\",\"description\":\"Install OpenSSH Server in the guest and deploy App Sandbox's key. Default false; run_command needs no SSH.\"},"
      "\"test_mode\":{\"type\":\"boolean\",\"description\":\"Windows: Secure Boot off and test signing on, to load test-signed drivers. Default false.\"}"
      "},\"required\":[\"name\",\"os_type\",\"admin_user\",\"admin_password\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_create_vm },
    { "start_vm", "Start a VM",
      "Boot a stopped VM from its current disk state. Returns at once; call wait_for_vm until it is online. To "
      "boot from a snapshot instead, use restore_snapshot. The App Sandbox app opens the VM's window on this PC "
      "by itself when the guest's display is up.",
      VM_ONLY, FALSE, FALSE, TRUE, FALSE, TRUE, tool_start_vm },
    { "shutdown_vm", "Shut a VM down",
      "Ask the guest OS to shut down cleanly (what you want before a snapshot). Returns at once; call wait_for_vm "
      "with until \"stopped\". Use stop_vm if the guest is hung or has no agent.",
      VM_ONLY, FALSE, FALSE, TRUE, FALSE, TRUE, tool_shutdown_vm },
    { "stop_vm", "Power a VM off",
      "Force a VM off immediately, like pulling the plug: unsaved guest data is lost and the guest file system "
      "may need checking. Use it when shutdown_vm does not work (hung or crashed guest).",
      VM_ONLY, FALSE, TRUE, TRUE, FALSE, TRUE, tool_stop_vm },
    { "delete_vm", "Delete a VM",
      "Permanently delete a VM with its disk and all its snapshots. This cannot be undone; only do it when the "
      "user asked for it or the VM was created by you for this task.",
      VM_ONLY, FALSE, TRUE, FALSE, FALSE, TRUE, tool_delete_vm },
    { "wait_for_vm", "Wait for a VM",
      "Block until a VM reaches a state, for at most timeout_seconds (default 90, at most 100), then report where "
      "it is. until \"online\" (default): the guest agent answers, so run_command works; \"stopped\": powered "
      "off; \"restart\": the guest goes offline and comes back online (after a reboot or a crash). Returns "
      "reached true/false, the state, install progress and what to do next; call it again while a new VM is "
      "still installing.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"until\":{\"type\":\"string\",\"enum\":[\"online\",\"stopped\",\"restart\"],\"description\":\"State to wait for. Default online.\"},"
      "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"description\":\"Longest wait. Default 90.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      TRUE, FALSE, TRUE, FALSE, FALSE, tool_wait_for_vm },
    { "run_command", "Run a command in a VM",
      "Run a command inside the guest and return its exit code, stdout and stderr. It runs as SYSTEM "
      "(through cmd.exe /c by default; shell \"powershell\" runs the text as a PowerShell script, \"none\" "
      "starts the program directly), without a visible window or desktop "
      "session. It works without network, through the guest agent. The VM must be online (wait_for_vm). If the "
      "connection breaks, the guest restarted or crashed: call wait_for_vm, then get_guest_crash. A non-zero exit "
      "code is reported, not an error. To start a program with a window, pass desktop true (it then runs as the "
      "signed-in user).",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"command\":{\"type\":\"string\",\"description\":\"The command line, e.g. pnputil /add-driver C:\\\\Drivers\\\\my.inf /install\"},"
      "\"shell\":{\"type\":\"string\",\"enum\":[\"cmd\",\"powershell\",\"none\"],\"description\":\"Default cmd.\"},"
      "\"working_dir\":{\"type\":\"string\",\"description\":\"Folder in the guest to run in. Default: the system folder.\"},"
      "\"desktop\":{\"type\":\"boolean\",\"description\":\"Run it as the signed-in user in their desktop session, so it can show windows (start notepad.exe, an installer, a test GUI). Programs it starts keep running after the command returns. Default false: as SYSTEM, without a desktop.\"},"
      "\"elevated\":{\"type\":\"boolean\",\"description\":\"With desktop: use the user's administrator rights (elevated) when the user is an administrator. Default false.\"},"
      "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600,\"description\":\"The command is ended after this long. Default 60.\"},"
      "\"max_output_bytes\":{\"type\":\"integer\",\"minimum\":256,\"maximum\":200000,\"description\":\"Most bytes kept of stdout and of stderr. Default 16000.\"}"
      "},\"required\":[\"vm\",\"command\"],\"additionalProperties\":false}",
      FALSE, TRUE, FALSE, TRUE, TRUE, tool_run_command },
    { "copy_to_guest", "Copy a file into a VM",
      "Write one file into the guest: from a file on this PC (host_path, up to 4 GB) or from content in the call "
      "(content_base64 or content_text, a few MB). Missing folders are created and an existing file is replaced. "
      "Use it to put a driver (.sys, .inf, .cat), a test program or a script into the VM. The VM must be online.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"guest_path\":{\"type\":\"string\",\"description\":\"Full destination path in the guest, e.g. C:\\\\Drivers\\\\my.sys\"},"
      "\"host_path\":{\"type\":\"string\",\"description\":\"Full path of the file on this PC. It is read with the user's own (non-elevated) rights.\"},"
      "\"content_base64\":{\"type\":\"string\",\"description\":\"The file's bytes as base64, instead of host_path.\"},"
      "\"content_text\":{\"type\":\"string\",\"description\":\"The file's text (written as UTF-8), instead of host_path.\"},"
      "\"content\":{\"type\":\"string\",\"description\":\"Same as content_text.\"}"
      "},\"required\":[\"vm\",\"guest_path\"],\"additionalProperties\":false}",
      FALSE, TRUE, TRUE, FALSE, TRUE, tool_copy_to_guest },
    { "copy_from_guest", "Copy a file out of a VM",
      "Copy one file from the guest to this PC and return its path there. Files go to the user's "
      "Downloads\\App Sandbox\\<vm name> folder (an existing file with the same name is replaced). Use it to "
      "fetch crash dumps (C:\\Windows\\Minidump\\*.dmp, C:\\Windows\\MEMORY.DMP), logs and test results. "
      "The VM must be online.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"guest_path\":{\"type\":\"string\",\"description\":\"Full path of the file in the guest.\"},"
      "\"save_as\":{\"type\":\"string\",\"description\":\"File name to save it under. Default: its name in the guest.\"},"
      "\"max_mb\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"description\":\"Refuse a larger file. Default 256.\"}"
      "},\"required\":[\"vm\",\"guest_path\"],\"additionalProperties\":false}",
      FALSE, FALSE, TRUE, FALSE, TRUE, tool_copy_from_guest },
    { "get_guest_crash", "Check a VM for crashes",
      "Find out whether the guest crashed: the recorded bugchecks (stop code and its four parameters, "
      "newest first), unexpected shutdowns, the crash dump files with their sizes and times, and the last boot "
      "time. Call it after a test, "
      "or after run_command lost its connection and wait_for_vm brought the VM back. Changes nothing.",
      VM_ONLY, TRUE, FALSE, TRUE, FALSE, FALSE, tool_get_guest_crash },
    { "screenshot", "Take a screenshot of a VM",
      "One picture of the guest's screen as a PNG image, scaled down to max_width. Works with the VM's window "
      "open or closed: it uses the display stream when that has a picture and otherwise copies the screen inside "
      "the guest (Windows guests with a signed-in user). Not available during OS installation. Use it to see "
      "dialogs, installers and error boxes that run_command cannot report, and to find where to click with "
      "send_input.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"max_width\":{\"type\":\"integer\",\"minimum\":320,\"maximum\":3840,\"description\":\"Widest picture returned, in pixels. Default 1280.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      TRUE, FALSE, TRUE, FALSE, FALSE, tool_screenshot },
    { "restart_vm", "Restart a VM's guest OS",
      "Restart the guest operating system (a clean reboot from inside, not a power cycle) and wait until its "
      "agent is online again, at most timeout_seconds. Driver work needs it often: after bcdedit changes, Driver "
      "Verifier settings, or a driver that loads at boot. Returns backOnline true/false.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"wait\":{\"type\":\"boolean\",\"description\":\"Wait for the guest to come back. Default true.\"},"
      "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":5,\"maximum\":100,\"description\":\"Longest wait. Default 90.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_restart_vm },
    { "install_driver", "Install a driver in a VM",
      "Install a Windows driver from this PC into the guest in one call: copies the package (the .inf's folder, "
      "or one .sys for a legacy driver) to C:\\Drivers\\<name>, checks test signing, Secure Boot and the files' "
      "signatures and says what is wrong, trusts the test certificate in the guest, then runs pnputil /add-driver "
      "/install (or sc create + sc start for a .sys). Returns the published oemNN.inf name, the devices using it "
      "with their problem codes, the tool output and warnings; it warns when the VM has no snapshot to go back "
      "to. A driver bug can crash the guest: take a snapshot first.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"host_path\":{\"type\":\"string\",\"description\":\"Full path on this PC of the .inf, of the package folder, or of a .sys (legacy driver).\"},"
      "\"service_name\":{\"type\":\"string\",\"description\":\"Legacy .sys only: name of the kernel service to create. Default: the file's name.\"},"
      "\"trust_certificate\":{\"type\":\"boolean\",\"description\":\"Add the package's signer to the guest's Root and TrustedPublisher stores when it is not trusted (test certificates). Default true.\"},"
      "\"check_only\":{\"type\":\"boolean\",\"description\":\"Copy and check the package but do not install it. Default false.\"}"
      "},\"required\":[\"vm\",\"host_path\"],\"additionalProperties\":false}",
      FALSE, TRUE, FALSE, FALSE, TRUE, tool_install_driver },
    { "uninstall_driver", "Remove a driver from a VM",
      "Remove a driver from the guest: published_name (oemNN.inf) deletes the package from the driver store and "
      "uninstalls it from its devices (pnputil /delete-driver /uninstall /force); service_name stops and deletes "
      "a legacy driver service. Returns the tool output.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"published_name\":{\"type\":\"string\",\"description\":\"The oemNN.inf name from install_driver or get_devices.\"},"
      "\"service_name\":{\"type\":\"string\",\"description\":\"Name of a legacy driver service to stop and delete.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, TRUE, TRUE, FALSE, TRUE, tool_uninstall_driver },
    { "get_devices", "List a VM's devices and drivers",
      "The guest's Plug and Play devices with their status and Device Manager problem code (by default only the "
      "ones with a problem), plus the third-party driver packages in the driver store (oemNN.inf, provider, "
      "version, signer). Use it to see whether a driver's device started. Windows guests.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"problem_only\":{\"type\":\"boolean\",\"description\":\"Only devices that are not working. Default true.\"},"
      "\"filter\":{\"type\":\"string\",\"description\":\"Only devices whose name, class or instance id contains this text.\"},"
      "\"max\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,\"description\":\"Most devices returned. Default 60.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      TRUE, FALSE, TRUE, FALSE, FALSE, tool_get_devices },
    { "get_debug_output", "Read a driver's debug output",
      "A driver's DbgPrint / KdPrint text from a Windows guest without a kernel debugger (the capture runs, "
      "but decoding real driver output has not been proven yet: if read reports undecodedEvents, show its "
      "undecodedSample to the user). action start begins "
      "capturing (kernel trace of DbgPrint events), read returns the lines printed since the last read, stop "
      "ends it. Only output that passes Windows' debug print filter is captured: DbgPrintEx(DPFLTR_IHVDRIVER_ID, "
      "DPFLTR_ERROR_LEVEL, ...) always passes; plain DbgPrint / KdPrint needs start with enable_filter true and "
      "then restart_vm. The capture ends at a guest restart: start again. providers adds ETW / WPP providers, "
      "saved as an .etl file returned by stop.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"read\",\"stop\",\"status\"],\"description\":\"What to do.\"},"
      "\"enable_filter\":{\"type\":\"boolean\",\"description\":\"With start: set the Debug Print Filter registry values so every DbgPrint level passes (takes effect after a restart). Default false.\"},"
      "\"providers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"With start: ETW provider GUIDs (e.g. the driver's WPP control GUID) to trace into an .etl file.\"},"
      "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":100000,\"description\":\"Most text returned; the newest lines are kept. Default 30000.\"}"
      "},\"required\":[\"vm\",\"action\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_get_debug_output },
    { "set_verifier", "Set Driver Verifier in a VM",
      "Turn Windows Driver Verifier on for named drivers (preset standard, or custom with flags), off (reset) or "
      "show its settings (query). Verifier makes the guest bugcheck at the first driver mistake (pool misuse, "
      "wrong IRQL, leaks at unload), which get_guest_crash and analyze_dump then explain. The settings apply "
      "after a restart: pass reboot true to restart now and wait. If the guest then crashes at every boot, "
      "stop_vm and restore_snapshot.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"drivers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Driver file names to verify, e.g. [\\\"mydrv.sys\\\"]. Not needed for reset and query.\"},"
      "\"preset\":{\"type\":\"string\",\"enum\":[\"standard\",\"custom\",\"reset\",\"query\"],\"description\":\"Default standard (custom when flags is given).\"},"
      "\"flags\":{\"type\":\"integer\",\"description\":\"For custom: the sum of the verifier option bits, e.g. 2363 for the standard set.\"},"
      "\"reboot\":{\"type\":\"boolean\",\"description\":\"Restart the guest now and wait for it. Default false.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, TRUE, TRUE, FALSE, TRUE, tool_set_verifier },
    { "analyze_dump", "Analyse a crash dump",
      "Run the Windows debugger's !analyze -v on a guest crash dump and return the essentials: bugcheck code, "
      "name and arguments, the process, the faulting module / image / symbol, the failure bucket, the call stack "
      "and the analysis text. By default it takes the newest minidump (else MEMORY.DMP); pass dump_path for "
      "another file (get_guest_crash lists them). The dump is copied to Downloads\\App Sandbox\\<vm> and the "
      "debugger (cdb.exe or kd.exe) runs on this PC without elevation; if this PC has none it uses one in the "
      "guest, and if there is none anywhere it says how to install it. Pass symbol_path (the folder with your "
      "driver's .pdb) to get your own function names. The first run downloads symbols and can take minutes.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"dump_path\":{\"type\":\"string\",\"description\":\"Full path of the dump in the guest. Default: the newest C:\\\\Windows\\\\Minidump\\\\*.dmp, else C:\\\\Windows\\\\MEMORY.DMP.\"},"
      "\"host_dump_path\":{\"type\":\"string\",\"description\":\"Instead of dump_path: a dump that is already on this PC (e.g. from copy_from_guest); the VM need not be running.\"},"
      "\"symbol_path\":{\"type\":\"string\",\"description\":\"Folder on this PC with the driver's .pdb files; Microsoft's symbol server is always added.\"},"
      "\"max_mb\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"description\":\"Refuse to copy a larger dump. Default 1024.\"},"
      "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":30,\"maximum\":600,\"description\":\"Time limit for the debugger. Default 300.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, FALSE, TRUE, TRUE, TRUE, tool_analyze_dump },
    { "send_input", "Type or click in a VM",
      "Send keyboard and mouse input to the guest's desktop: text (typed as key presses), keys (names or chords "
      "such as enter, ctrl+shift+esc, win+r; several separated by spaces), a click or move at x,y in guest pixels "
      "(the coordinates of a screenshot taken at full size), and scroll. When several are given they run in this "
      "order: mouse, scroll, text, keys. Text and keys go to whatever has the keyboard focus in the guest, which "
      "may not be the program you mean (something may have opened by itself): take a screenshot first, click "
      "the field you want, then type; the result's focusedWindow names the window that had the focus. Works with "
      "the VM's window open or closed; needs a signed-in user. Text assumes the guest uses the same keyboard "
      "layout as this PC. Ctrl+Alt+Del cannot be sent.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"text\":{\"type\":\"string\",\"description\":\"Text to type, up to 2000 characters; a line break presses Enter.\"},"
      "\"keys\":{\"type\":\"string\",\"description\":\"Keys or chords, space separated: enter tab esc space backspace delete home end pageup pagedown up down left right f1..f24 win, or one character; join with + (alt+f4).\"},"
      "\"x\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Horizontal position in guest pixels (with y).\"},"
      "\"y\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Vertical position in guest pixels (with x).\"},"
      "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\",\"none\"],\"description\":\"Button to click at x,y. Default left; none only moves the pointer.\"},"
      "\"double_click\":{\"type\":\"boolean\",\"description\":\"Click twice. Default false.\"},"
      "\"scroll\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100,\"description\":\"Wheel notches: positive scrolls up, negative down.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_send_input },
    { "list_snapshots", "List a VM's snapshots",
      "The VM's snapshot tree: each snapshot's index, name, parent and branches, and which one the VM's disk is "
      "currently based on. Indexes are what restore_snapshot and delete_snapshot take.",
      VM_ONLY, TRUE, FALSE, TRUE, FALSE, FALSE, tool_list_snapshots },
    { "take_snapshot", "Take a snapshot",
      "Save the VM's disk state as a snapshot to come back to. The VM must be stopped (shutdown_vm, then "
      "wait_for_vm until \"stopped\"). Take one before installing a driver or anything else that may break the "
      "guest.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"name\":{\"type\":\"string\",\"description\":\"A name for the snapshot, e.g. clean. Default: a numbered name.\"}"
      "},\"required\":[\"vm\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_take_snapshot },
    { "restore_snapshot", "Start a VM from a snapshot",
      "Go back to a snapshot: starts the stopped VM from that snapshot's disk state on a new branch. What the VM "
      "had after the snapshot is kept as another branch, not deleted. Follow with wait_for_vm.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"snapshot\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"The snapshot's index, from list_snapshots.\"},"
      "\"branch_name\":{\"type\":\"string\",\"description\":\"A name for the new branch. Default: a numbered name.\"}"
      "},\"required\":[\"vm\",\"snapshot\"],\"additionalProperties\":false}",
      FALSE, FALSE, FALSE, FALSE, TRUE, tool_restore_snapshot },
    { "delete_snapshot", "Delete a snapshot",
      "Permanently delete a snapshot and the branches made from it, freeing their disk space. The VM must be "
      "stopped. Cannot be undone.",
      "{\"type\":\"object\",\"properties\":{" VM_PROP ","
      "\"snapshot\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"The snapshot's index, from list_snapshots.\"}"
      "},\"required\":[\"vm\",\"snapshot\"],\"additionalProperties\":false}",
      FALSE, TRUE, FALSE, FALSE, TRUE, tool_delete_snapshot },
    { "ssh_info", "Get a VM's SSH address",
      "Where to reach the guest's SSH server from this PC: host, port, user and whether SSH is ready and App "
      "Sandbox's key is deployed. Only for VMs created with ssh; run_command does not need it.",
      VM_ONLY, TRUE, FALSE, TRUE, FALSE, FALSE, tool_ssh_info },
    { "open_display", "Open a VM's window",
      "Open the VM's screen in a window on this PC's desktop, for the user to watch or use (or bring it to the "
      "front). You cannot see that window; use screenshot to look at the guest yourself.",
      VM_ONLY, FALSE, FALSE, TRUE, FALSE, TRUE, tool_open_display },
    { "close_display", "Close a VM's window",
      "Close the VM's window on this PC's desktop. The VM keeps running.",
      VM_ONLY, FALSE, FALSE, TRUE, FALSE, TRUE, tool_close_display },
    { "get_log", "Read App Sandbox's activity log",
      "The most recent lines of App Sandbox's activity log on this PC, optionally only those about one VM: "
      "starts and stops, installation steps, guest agent connections, guest crash reports and errors. Read it "
      "when a tool failed or a VM does not come online.",
      "{\"type\":\"object\",\"properties\":{"
      "\"vm\":{\"type\":\"string\",\"description\":\"Only lines that mention this VM (name or id). Default: all.\"},"
      "\"lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":400,\"description\":\"How many lines, newest last. Default 80.\"}"
      "},\"additionalProperties\":false}",
      TRUE, FALSE, TRUE, FALSE, FALSE, tool_get_log },
};

static const char g_instructions[] =
    "App Sandbox runs disposable, GPU-capable Windows 11 virtual machines on this PC. Use a VM to "
    "try software, installers, drivers and other risky or low-level things instead of the real machine. Call "
    "list_vms first (get_host shows capacity and templates). Usual flow: create_vm or start_vm, then "
    "wait_for_vm until online, then run_command (runs as SYSTEM), copy_to_guest and copy_from_guest. "
    "Take a snapshot before a risky step and restore it to go back.\n"
    "\n"
    "Details:\n"
    "- Every VM argument is \"vm\": the VM's name or id.\n"
    "- States: stopped, building, installing, booting, online, stopping. A new VM installs its OS unattended "
    "(10 to 40 minutes): keep calling wait_for_vm until it is online. Tools return at once unless they say "
    "they wait.\n"
    "- The guest tools (run_command, copy_*, install_driver, get_debug_output, get_guest_crash...) go through "
    "the guest agent: no network or SSH needed, but the VM must be online. If one says the guest agent is too "
    "old, shut the VM down and start it again (the guest components are updated before it starts).\n"
    "- Snapshots need a stopped VM: shutdown_vm, wait_for_vm until \"stopped\", take_snapshot, start_vm. "
    "restore_snapshot starts the VM from a snapshot on a new branch. No tool takes a snapshot of a running VM "
    "for you: take one before installing a driver.\n"
    "- screenshot shows the guest's screen and send_input types and clicks in it; get_log shows what App "
    "Sandbox did and why something failed.\n"
    "\n"
    "Testing a Windows driver (prompt test_driver has the full loop):\n"
    "1. Use a VM created with test_mode true (Secure Boot off, test signing on). With it stopped, take_snapshot "
    "(name it clean), then start_vm and wait_for_vm.\n"
    "2. get_debug_output action start, then install_driver with the .inf (or .sys) on this PC: it copies the "
    "package, checks signing, installs it and returns the devices and their problem codes.\n"
    "3. Exercise the driver with run_command; get_debug_output action read returns its DbgPrint text. "
    "set_verifier (reboot true) turns Driver Verifier on; restart_vm reboots the guest.\n"
    "4. If a call loses its connection or the guest restarts, it probably bugchecked: wait_for_vm until "
    "\"online\", then get_guest_crash (stop code, dumps) and analyze_dump (faulting module, stack).\n"
    "5. To retry from a clean state: stop_vm, wait_for_vm until \"stopped\", restore_snapshot with the clean "
    "snapshot's index (list_snapshots), wait_for_vm, and go to step 2.\n"
    "\n"
    "Limits: VMs stop when App Sandbox exits. run_command has no desktop unless desktop is true. If tools that "
    "change things are missing, the user has put this server in read-only mode (App Sandbox > Settings > AI "
    "agents).";

/* ---------------------------------------------------------------- prompts */

typedef struct {
    const char *name, *title, *description;
    const char *arguments;   /* JSON array of {name, description, required} */
    const char *text;        /* %VM% and %DRIVER% stand for the arguments */
} Prompt;

static const Prompt g_prompts[] = {
    { "test_driver", "Test a Windows driver in a VM",
      "Install a driver you built into an App Sandbox VM, exercise it, and diagnose a crash, with a snapshot to go back to.",
      "[{\"name\":\"vm\",\"description\":\"Name of the VM to test in (see list_vms). Use a VM made for testing, created with test mode.\",\"required\":true},"
      "{\"name\":\"driver\",\"description\":\"Path on this PC of the driver's .inf file, package folder or .sys.\",\"required\":true}]",
      "Test the Windows driver at %DRIVER% in the App Sandbox VM \"%VM%\", using the appsandbox MCP tools. "
      "Work through these steps and tell me what you find at each one.\n"
      "\n"
      "1. Check the VM: get_vm. It should be a Windows VM with testMode true (Secure Boot off, test signing on); "
      "if it is not, say so before going on, because a test-signed driver will not load.\n"
      "2. Make a way back: list_snapshots. If there is no clean snapshot, shutdown_vm, wait_for_vm until "
      "\"stopped\", take_snapshot named \"clean\", then start_vm and wait_for_vm until online. Remember the "
      "snapshot's index.\n"
      "3. Start capturing debug output: get_debug_output with action \"start\".\n"
      "4. install_driver with host_path %DRIVER%. Read warnings first: they say what is wrong with signing. "
      "Report publishedName and every device's problemCode (0 means it started).\n"
      "5. Exercise the driver with run_command (start its service or test program, or use get_devices to see "
      "its device), then get_debug_output with action \"read\" for its DbgPrint output.\n"
      "6. Optional stress: set_verifier with drivers [the .sys name], preset \"standard\", reboot true; then "
      "start get_debug_output again and repeat step 5.\n"
      "7. If a call loses its connection, or the guest restarts: wait_for_vm until \"online\", then "
      "get_guest_crash. If there is a new bugcheck, analyze_dump (pass symbol_path, the folder with the "
      "driver's .pdb) and explain the faulting module, the stack and the likely bug in the source.\n"
      "8. To try a fixed build from a clean state: stop_vm, wait_for_vm until \"stopped\", restore_snapshot "
      "with the clean snapshot's index, wait_for_vm, and go back to step 3.\n"
      "\n"
      "If the guest does not come back after a restart, take a screenshot to see why (a crash at boot shows a "
      "recovery screen), then stop_vm and restore_snapshot. Do not delete the VM or its snapshots." },
    { "new_test_vm", "Create a VM for driver testing",
      "Create a Windows VM set up for testing drivers, with a clean snapshot.",
      "[{\"name\":\"vm\",\"description\":\"Name for the new VM: letters, digits and hyphens, at most 15 characters.\",\"required\":true}]",
      "Create a Windows VM called \"%VM%\" in App Sandbox for testing drivers, using the appsandbox MCP tools.\n"
      "\n"
      "1. get_host: note the free disk space, the GPUs and the templates. If a Windows template exists, use it "
      "(much faster); otherwise ask me for the path of a Windows 11 ISO on this PC.\n"
      "2. Ask me for the administrator user name and password to create in the guest, unless I gave them.\n"
      "3. create_vm with os_type \"Windows\", test_mode true (Secure Boot off and test signing on, needed for "
      "test-signed drivers), and sizes that fit this PC (4 cores, 8192 MB RAM and 64 GB disk are enough).\n"
      "4. wait_for_vm until it is online. Installing takes 10 to 40 minutes: keep calling it while it says "
      "installing, and tell me the progress now and then.\n"
      "5. Check it: run_command \"bcdedit /enum {current}\" should show testsigning Yes.\n"
      "6. Make the clean snapshot: shutdown_vm, wait_for_vm until \"stopped\", take_snapshot named \"clean\", "
      "then start_vm and wait_for_vm until online.\n"
      "7. Tell me the VM is ready and that the test_driver prompt (or install_driver) is the next step." },
};

static const Tool *find_tool(const char *name)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(g_tools); i++)
        if (strcmp(g_tools[i].name, name) == 0) return &g_tools[i];
    return NULL;
}

/* ---------------------------------------------------------------- JSON-RPC */

static void put_id(Sb *out, const JVal *id)
{
    if (id && id->type == J_STR) sb_json_n(out, id->str, id->len);
    else if (id && id->type == J_NUM) sb_puts(out, id->str);
    else sb_puts(out, "null");
}

static void put_server_info(Sb *out)
{
    sb_printf(out, "{\"name\":\"%s\",\"title\":\"App Sandbox\",\"version\":\"%s\"}",
              MCP_SERVER_NAME, asb_api_product_version());
}

static void put_versions(Sb *out, BOOL with_legacy)
{
    size_t i;
    sb_puts(out, "[\"" MCP_MODERN "\"");
    for (i = ARRAYSIZE(g_legacy_versions); with_legacy && i > 0; i--)
        sb_printf(out, ",\"%s\"", g_legacy_versions[i - 1]);
    sb_puts(out, "]");
}

static void rpc_error(Sb *out, const JVal *id, int code, const char *message, const char *data_json)
{
    out->len = 0;
    sb_puts(out, "{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(out, id);
    sb_printf(out, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json(out, message);
    if (data_json) { sb_puts(out, ",\"data\":"); sb_puts(out, data_json); }
    sb_puts(out, "}}");
}

static void put_tools(Sb *out)
{
    BOOL read_only = asb_get_mcp_read_only(), first = TRUE;
    size_t i;
    sb_puts(out, "[");
    for (i = 0; i < ARRAYSIZE(g_tools); i++) {
        const Tool *tool = &g_tools[i];
        if (read_only && tool->changes) continue;
        sb_printf(out, "%s{\"name\":\"%s\",\"title\":", first ? "" : ",", tool->name);
        sb_json(out, tool->title);
        sb_puts(out, ",\"description\":");
        sb_json(out, tool->description);
        sb_puts(out, ",\"inputSchema\":");
        sb_puts(out, tool->schema);
        sb_printf(out, ",\"annotations\":{\"title\":");
        sb_json(out, tool->title);
        sb_printf(out, ",\"readOnlyHint\":%s,\"destructiveHint\":%s,\"idempotentHint\":%s,\"openWorldHint\":%s}}",
                  tool->read_only ? "true" : "false", tool->destructive ? "true" : "false",
                  tool->idempotent ? "true" : "false", tool->open_world ? "true" : "false");
        first = FALSE;
    }
    sb_puts(out, "]");
}

/* Answer one JSON-RPC message. Returns the HTTP status; 202 means a
   notification (no body). */
static int rpc_handle(const char *body, size_t len, Sb *out)
{
    JVal *msg = json_parse(body, len);
    const JVal *id, *params, *meta;
    const char *method, *wanted;
    BOOL modern = FALSE;
    int status = 200;

    if (!msg) {
        rpc_error(out, NULL, -32700, "Parse error: the request body is not valid JSON.", NULL);
        return 400;
    }
    id = j_get(msg, "id");
    method = j_str(msg, "method");
    params = j_get(msg, "params");
    if (msg->type != J_OBJ || !method) {
        /* A response to a request this server never sends, or a batch. */
        if (msg->type == J_OBJ && (j_get(msg, "result") || j_get(msg, "error"))) status = 202;
        else { rpc_error(out, id, -32600, "Invalid request: expected one JSON-RPC request object.", NULL); status = 400; }
        json_free(msg);
        return status;
    }
    if (!id) {   /* notification */
        json_free(msg);
        return 202;
    }

    /* The stateless protocol names its version in every request. */
    meta = j_get(params, "_meta");
    wanted = j_str(meta, MCP_META_VERSION);
    if (wanted) {
        if (strcmp(wanted, MCP_MODERN) != 0) {
            Sb data = { 0 };
            sb_puts(&data, "{\"supported\":");
            put_versions(&data, TRUE);
            sb_puts(&data, ",\"requested\":");
            sb_json(&data, wanted);
            sb_puts(&data, "}");
            rpc_error(out, id, -32022, "Unsupported protocol version.", data.failed ? NULL : data.p);
            sb_free(&data);
            json_free(msg);
            return 400;
        }
        modern = TRUE;
        note_client(j_str(j_get(meta, MCP_META_CLIENT), "name"));
    }

    sb_puts(out, "{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(out, id);
    sb_puts(out, ",\"result\":");

    if (!strcmp(method, "initialize")) {
        const char *requested = j_str(params, "protocolVersion"), *version = MCP_LATEST_LEGACY;
        size_t i;
        for (i = 0; requested && i < ARRAYSIZE(g_legacy_versions); i++)
            if (!strcmp(requested, g_legacy_versions[i])) version = g_legacy_versions[i];
        note_client(j_str(j_get(params, "clientInfo"), "name"));
        sb_printf(out, "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false},"
                       "\"prompts\":{\"listChanged\":false}},\"serverInfo\":", version);
        put_server_info(out);
        sb_puts(out, ",\"instructions\":");
        sb_json(out, g_instructions);
        sb_puts(out, "}");
    } else if (!strcmp(method, "ping")) {
        sb_puts(out, modern ? "{\"resultType\":\"complete\"}" : "{}");
    } else if (!strcmp(method, "server/discover")) {
        sb_puts(out, "{\"supportedVersions\":");
        put_versions(out, TRUE);
        sb_puts(out, ",\"capabilities\":{\"tools\":{\"listChanged\":false},\"prompts\":{\"listChanged\":false}},\"instructions\":");
        sb_json(out, g_instructions);
        sb_puts(out, ",\"ttlMs\":300000,\"cacheScope\":\"public\",\"resultType\":\"complete\",\"_meta\":{\"" MCP_META_SERVER "\":");
        put_server_info(out);
        sb_puts(out, "}}");
    } else if (!strcmp(method, "tools/list")) {
        sb_puts(out, "{\"tools\":");
        put_tools(out);
        /* Short-lived: read-only mode changes the list. */
        if (modern) sb_puts(out, ",\"ttlMs\":30000,\"cacheScope\":\"public\",\"resultType\":\"complete\"");
        sb_puts(out, "}");
    } else if (!strcmp(method, "resources/list") || !strcmp(method, "resources/templates/list")) {
        sb_printf(out, "{\"%s\":[]%s}", !strcmp(method, "resources/list") ? "resources" : "resourceTemplates",
                  modern ? ",\"ttlMs\":300000,\"cacheScope\":\"public\",\"resultType\":\"complete\"" : "");
    } else if (!strcmp(method, "prompts/list")) {
        size_t i;
        sb_puts(out, "{\"prompts\":[");
        for (i = 0; i < ARRAYSIZE(g_prompts); i++) {
            sb_printf(out, "%s{\"name\":\"%s\",\"title\":", i ? "," : "", g_prompts[i].name);
            sb_json(out, g_prompts[i].title);
            sb_puts(out, ",\"description\":");
            sb_json(out, g_prompts[i].description);
            sb_puts(out, ",\"arguments\":");
            sb_puts(out, g_prompts[i].arguments);
            sb_puts(out, "}");
        }
        sb_puts(out, modern ? "],\"ttlMs\":300000,\"cacheScope\":\"public\",\"resultType\":\"complete\"}" : "]}");
    } else if (!strcmp(method, "prompts/get")) {
        /* A prompt is text for the agent to follow with the tools; it runs
           nothing itself, so read-only mode needs no check here. */
        const char *name = j_str(params, "name"), *vm = j_str(j_get(params, "arguments"), "vm");
        const char *driver = j_str(j_get(params, "arguments"), "driver"), *p;
        const Prompt *prompt = NULL;
        size_t i;
        for (i = 0; name && i < ARRAYSIZE(g_prompts); i++)
            if (!strcmp(name, g_prompts[i].name)) prompt = &g_prompts[i];
        if (!prompt) {
            rpc_error(out, id, -32602, "Unknown prompt. Call prompts/list for the prompts this server has.", NULL);
            json_free(msg);
            return status;
        } else {
            Sb text = { 0 };
            for (p = prompt->text; *p; ) {
                if (!strncmp(p, "%VM%", 4)) { sb_puts(&text, vm && vm[0] ? vm : "(ask me which VM)"); p += 4; }
                else if (!strncmp(p, "%DRIVER%", 8)) { sb_puts(&text, driver && driver[0] ? driver : "(ask me for the driver's path)"); p += 8; }
                else { sb_put(&text, p, 1); p++; }
            }
            sb_puts(out, "{\"description\":");
            sb_json(out, prompt->description);
            sb_puts(out, ",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":");
            sb_json(out, text.p ? text.p : "");
            sb_puts(out, modern ? "}}],\"resultType\":\"complete\"}" : "}}]}");
            sb_free(&text);
        }
    } else if (!strcmp(method, "tools/call")) {
        const char *name = j_str(params, "name");
        const JVal *args = j_get(params, "arguments");
        const Tool *tool = name ? find_tool(name) : NULL;
        if (!tool) {
            rpc_error(out, id, -32602, name ? "Unknown tool. Call tools/list for the tools this server has."
                                            : "tools/call needs params.name.", NULL);
        } else {
            ToolCall call;
            const char *vm_arg = j_str(args, "vm");
            memset(&call, 0, sizeof(call));
            call_vm(&call, vm_arg);
            if (tool->changes && asb_get_mcp_read_only())
                result_error(&call, "%s is turned off: the user has put App Sandbox's MCP server in read-only mode "
                                    "(App Sandbox > Settings > AI agents (MCP)). Ask the user to turn "
                                    "read-only off if this change is wanted.", tool->name);
            else
                tool->run(&call, args && args->type == J_OBJ ? args : NULL);
            if (!call.out.len && !call.out.failed) result_error(&call, "The tool produced no answer.");
            note_activity(tool->name, call.vm, !call.is_error);
            if (tool->changes)   /* audit line in the host log */
                ui_log(L"MCP: %S%S%S%S -> %s", tool->name, call.vm[0] ? " (" : "", call.vm, call.vm[0] ? ")" : "",
                       call.is_error ? L"failed" : L"ok");
            if (call.out.failed) {
                rpc_error(out, id, -32603, "Out of memory while building the answer.", NULL);
            } else {
                sb_put(out, call.out.p, call.out.len);
                sb_puts(out, modern ? ",\"resultType\":\"complete\"}}" : "}}");
            }
            sb_free(&call.out);
        }
        json_free(msg);
        return status;   /* the envelope is closed below for the other methods */
    } else {
        rpc_error(out, id, -32601, "Method not found.", NULL);
        json_free(msg);
        return status;
    }
    sb_puts(out, "}");
    json_free(msg);
    return status;
}

/* ---------------------------------------------------------------- HTTP */

static void http_respond(HTTP_REQUEST_ID id, USHORT status, const char *reason, const char *body, size_t len,
                         const char *allow)
{
    HTTP_RESPONSE resp;
    HTTP_DATA_CHUNK chunk;
    ULONG sent = 0;
    HANDLE queue = (HANDLE)asb_api_queue();

    if (!queue) return;
    RtlZeroMemory(&resp, sizeof(resp));
    resp.StatusCode = status;
    resp.pReason = reason; resp.ReasonLength = (USHORT)strlen(reason);
    if (allow) {
        resp.Headers.KnownHeaders[HttpHeaderAllow].pRawValue = allow;
        resp.Headers.KnownHeaders[HttpHeaderAllow].RawValueLength = (USHORT)strlen(allow);
    }
    if (body && len) {
        resp.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "application/json";
        resp.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen("application/json");
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = (PVOID)body;
        chunk.FromMemory.BufferLength = (ULONG)len;
        resp.EntityChunkCount = 1; resp.pEntityChunks = &chunk;
    }
    HttpSendHttpResponse(queue, id, 0, &resp, NULL, &sent, NULL, 0, NULL, NULL);
}

static void http_refuse(HTTP_REQUEST_ID id, USHORT status, const char *reason, const char *message, const char *allow)
{
    char body[300];
    int n = sprintf_s(body, sizeof(body), "{\"error\":\"%s\"}", message);
    http_respond(id, status, reason, body, n > 0 ? (size_t)n : 0, allow);
}

/* TRUE when `host` (n bytes, an optional :port) names this computer's loopback. */
static BOOL loopback_host(const char *host, size_t n)
{
    static const char *const names[] = { "127.0.0.1", "localhost", "[::1]" };
    size_t i, name_len = n;
    if (n && host[0] == '[') {
        const char *close = (const char *)memchr(host, ']', n);
        if (!close) return FALSE;
        name_len = (size_t)(close - host) + 1;
        if (name_len < n && host[name_len] != ':') return FALSE;
    } else {
        const char *colon = (const char *)memchr(host, ':', n);
        if (colon) name_len = (size_t)(colon - host);
    }
    for (i = 0; i < ARRAYSIZE(names); i++)
        if (strlen(names[i]) == name_len && _strnicmp(host, names[i], name_len) == 0) return TRUE;
    return FALSE;
}

/* TRUE when an Origin header value is a page served from this computer. */
static BOOL loopback_origin(const char *origin, size_t n)
{
    size_t skip, end;
    if (n >= 7 && _strnicmp(origin, "http://", 7) == 0) skip = 7;
    else if (n >= 8 && _strnicmp(origin, "https://", 8) == 0) skip = 8;
    else return FALSE;
    for (end = skip; end < n && origin[end] != '/'; end++) {}
    return loopback_host(origin + skip, end - skip);
}

BOOL mcp_owns_request(const void *request)
{
    const HTTP_REQUEST *req = (const HTTP_REQUEST *)request;
    const SOCKADDR *local = req->Address.pLocalAddress;
    if (!local) return FALSE;
    if (local->sa_family == AF_INET) return ((const SOCKADDR_IN *)local)->sin_port == htons(MCP_PORT);
    if (local->sa_family == AF_INET6) return ((const SOCKADDR_IN6 *)local)->sin6_port == htons(MCP_PORT);
    return FALSE;
}

typedef struct {
    HTTP_REQUEST_ID id;
    char  *body;
    size_t len;
} McpJob;

static DWORD WINAPI mcp_worker(LPVOID param)
{
    McpJob *job = (McpJob *)param;
    Sb out = { 0 };
    int status = rpc_handle(job->body, job->len, &out);
    if (status == 202)
        http_respond(job->id, 202, "Accepted", NULL, 0, NULL);
    else if (out.failed || !out.p)
        http_refuse(job->id, 500, "Internal Server Error", "out of memory", NULL);
    else
        http_respond(job->id, (USHORT)status, status == 200 ? "OK" : "Bad Request", out.p, out.len, NULL);
    sb_free(&out);
    free(job->body);
    free(job);
    InterlockedDecrement(&g_workers);
    return 0;
}

void mcp_handle_http(const void *request)
{
    const HTTP_REQUEST *req = (const HTTP_REQUEST *)request;
    const HTTP_KNOWN_HEADER *host = &req->Headers.KnownHeaders[HttpHeaderHost];
    const HTTP_KNOWN_HEADER *ctype = &req->Headers.KnownHeaders[HttpHeaderContentType];
    const wchar_t *path = req->CookedUrl.pAbsPath;
    size_t path_len = req->CookedUrl.AbsPathLength / sizeof(wchar_t);
    HANDLE queue = (HANDLE)asb_api_queue();
    McpJob *job;
    HANDLE thread;
    char *body = NULL;
    size_t len = 0, cap = 0;
    USHORT i;

    mcp_init();
    /* Web pages must not reach this server: a page can make the browser send
       requests to 127.0.0.1, but it cannot fake Host (DNS rebinding) or
       Origin, and it cannot send application/json without a preflight. */
    if (!host->pRawValue || !loopback_host(host->pRawValue, host->RawValueLength)) {
        http_refuse(req->RequestId, 403, "Forbidden", "this server only answers on 127.0.0.1", NULL);
        return;
    }
    for (i = 0; i < req->Headers.UnknownHeaderCount; i++) {
        const HTTP_UNKNOWN_HEADER *h = &req->Headers.pUnknownHeaders[i];
        if (h->NameLength == 6 && _strnicmp(h->pName, "Origin", 6) == 0 &&
            !loopback_origin(h->pRawValue, h->RawValueLength)) {
            http_refuse(req->RequestId, 403, "Forbidden", "requests from web pages are not accepted", NULL);
            return;
        }
    }
    if (!path || !((path_len == 4 && !wcsncmp(path, L"/mcp", 4)) || (path_len == 5 && !wcsncmp(path, L"/mcp/", 5)))) {
        http_refuse(req->RequestId, 404, "Not Found", "the MCP endpoint is POST /mcp", NULL);
        return;
    }
    if (req->Verb != HttpVerbPOST) {
        http_refuse(req->RequestId, 405, "Method Not Allowed", "use POST: one JSON-RPC message per request", "POST");
        return;
    }
    if (!ctype->pRawValue || ctype->RawValueLength < 16 ||
        _strnicmp(ctype->pRawValue, "application/json", 16) != 0 ||
        (ctype->RawValueLength > 16 && ctype->pRawValue[16] != ';' && ctype->pRawValue[16] != ' ')) {
        http_refuse(req->RequestId, 415, "Unsupported Media Type", "Content-Type must be application/json", NULL);
        return;
    }
    if (g_stopping || !queue) {
        http_refuse(req->RequestId, 503, "Service Unavailable", "App Sandbox is closing", NULL);
        return;
    }

    /* The message is read here, in order, and answered on its own thread. */
    for (;;) {
        ULONG got = 0, r;
        if (cap - len < 65536) {
            size_t grown_cap = cap ? cap * 2 : 131072;
            char *grown = (char *)realloc(body, grown_cap + 1);
            if (!grown) { free(body); http_refuse(req->RequestId, 500, "Internal Server Error", "out of memory", NULL); return; }
            body = grown; cap = grown_cap;
        }
        r = HttpReceiveRequestEntityBody(queue, req->RequestId, 0, body + len, (ULONG)(cap - len), &got, NULL);
        len += got;
        if (r == ERROR_HANDLE_EOF) break;
        if (r != NO_ERROR) { free(body); return; }   /* the client went away */
        if (len > MCP_MAX_BODY) {
            free(body);
            http_refuse(req->RequestId, 413, "Payload Too Large", "the request is larger than 8 MB", NULL);
            return;
        }
    }
    body[len] = '\0';

    job = (McpJob *)malloc(sizeof(*job));
    if (!job) { free(body); http_refuse(req->RequestId, 500, "Internal Server Error", "out of memory", NULL); return; }
    job->id = req->RequestId; job->body = body; job->len = len;
    if (InterlockedIncrement(&g_workers) > MCP_MAX_WORKERS) {
        InterlockedDecrement(&g_workers);
        free(body); free(job);
        http_refuse(req->RequestId, 503, "Service Unavailable", "too many requests at the same time; retry shortly", NULL);
        return;
    }
    thread = CreateThread(NULL, 0, mcp_worker, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedDecrement(&g_workers);
        free(body); free(job);
        http_refuse(req->RequestId, 503, "Service Unavailable", "could not start a worker thread", NULL);
    }
}
