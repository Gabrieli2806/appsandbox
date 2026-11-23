/*
 * ui_bridge.c - Messages between the host (ui.c) and the app shell, and the
 * small JSON builder and reader both ui.c and headless.c use.
 */

#include "ui_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUED 256

static UiBridgeCallback g_callback;
static wchar_t *g_queue[MAX_QUEUED];
static int g_queue_count;

void ui_bridge_set_callback(UiBridgeCallback cb)
{
    int i, count;
    g_callback = cb;
    if (!cb) return;
    /* Deliver what was posted before the shell was listening, in order. The
       callback may post again; those go straight through. */
    count = g_queue_count;
    g_queue_count = 0;
    for (i = 0; i < count; i++) {
        cb(g_queue[i]);
        free(g_queue[i]);
        g_queue[i] = NULL;
    }
}

void ui_bridge_post(const wchar_t *json)
{
    if (!json) return;
    if (g_callback) {
        g_callback(json);
    } else if (g_queue_count < MAX_QUEUED) {
        size_t len = wcslen(json) + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            memcpy(copy, json, len * sizeof(wchar_t));
            g_queue[g_queue_count++] = copy;
        }
    }
}

void ui_bridge_cleanup(void)
{
    int i;
    g_callback = NULL;
    for (i = 0; i < g_queue_count; i++) free(g_queue[i]);
    g_queue_count = 0;
}

/* ---- JSON builder ---- */

void jb_init(JsonBuilder *jb, wchar_t *buf, size_t cap)
{
    jb->buf = buf;
    jb->cap = cap;
    jb->len = 0;
    jb->count = 0;
    jb->truncated = FALSE;
    if (cap > 0) buf[0] = L'\0';
}

void jb_append(JsonBuilder *jb, const wchar_t *s)
{
    size_t slen = wcslen(s);
    if (jb->len + slen < jb->cap) {
        wcscpy_s(jb->buf + jb->len, jb->cap - jb->len, s);
        jb->len += slen;
    } else {
        jb->truncated = TRUE;
    }
}

void jb_append_escaped(JsonBuilder *jb, const wchar_t *s)
{
    const wchar_t *p;
    for (p = s; *p; p++) {
        if (jb->len + 6 >= jb->cap) { jb->truncated = TRUE; break; }
        switch (*p) {
        case L'\\': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'\\'; break;
        case L'"':  jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'"'; break;
        case L'\n': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'n'; break;
        case L'\r': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'r'; break;
        case L'\t': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L't'; break;
        default:
            /* Other control characters are not valid in a JSON string, and
               one of them makes WebView2 reject the whole message. */
            if (*p < 0x20) {
                int n = swprintf_s(jb->buf + jb->len, jb->cap - jb->len, L"\\u%04x", (unsigned)*p);
                if (n > 0) jb->len += (size_t)n;
            } else {
                jb->buf[jb->len++] = *p;
            }
            break;
        }
    }
    jb->buf[jb->len] = L'\0';
}

void jb_object_begin(JsonBuilder *jb)
{
    jb_append(jb, L"{");
    jb->count = 0;
}

void jb_object_end(JsonBuilder *jb)
{
    jb_append(jb, L"}");
}

void jb_array_begin(JsonBuilder *jb, const wchar_t *key)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":[");
    jb->count = 0;
}

void jb_array_end(JsonBuilder *jb)
{
    jb_append(jb, L"]");
    jb->count = 1; /* so next field gets a comma */
}

void jb_string(JsonBuilder *jb, const wchar_t *key, const wchar_t *val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":\"");
    jb_append_escaped(jb, val ? val : L"");
    jb_append(jb, L"\"");
    jb->count++;
}

void jb_int(JsonBuilder *jb, const wchar_t *key, int val)
{
    wchar_t tmp[32];
    if (jb->count > 0) jb_append(jb, L",");
    swprintf_s(tmp, 32, L"\"%s\":%d", key, val);
    jb_append(jb, tmp);
    jb->count++;
}

void jb_bool(JsonBuilder *jb, const wchar_t *key, BOOL val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, val ? L"\":true" : L"\":false");
    jb->count++;
}

/* ---- Simple JSON parser ---- */

/* Skip quoted values so they cannot be mistaken for property names. */
static const wchar_t *json_find_value(const wchar_t *json, const wchar_t *key)
{
    const wchar_t *p = json;
    size_t key_len;
    if (!json || !key) return NULL;
    key_len = wcslen(key);
    while (*p) {
        const wchar_t *start, *end;
        if (*p != L'"') { p++; continue; }
        start = ++p;
        while (*p && *p != L'"') {
            if (*p == L'\\' && p[1]) p += 2;
            else p++;
        }
        if (!*p) return NULL;
        end = p++;
        while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') p++;
        if (*p != L':' || (size_t)(end - start) != key_len ||
            wcsncmp(start, key, key_len) != 0) continue;
        p++;
        while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') p++;
        return p;
    }
    return NULL;
}

BOOL json_has_key(const wchar_t *json, const wchar_t *key)
{
    return json_find_value(json, key) != NULL;
}

BOOL json_get_string(const wchar_t *json, const wchar_t *key,
                     wchar_t *out, size_t out_len)
{
    const wchar_t *p = json_find_value(json, key);
    size_t o = 0;

    if (!p || !out || !out_len || *p != L'"') return FALSE;
    p++;

    /* Copy the value into `out`, DECODING JSON string escapes. \uXXXX -- what
       json.dumps/ensure_ascii and many HTTP clients emit for non-ASCII -- and \" \\ are
       decoded rather than stored as literal text, so e.g. non-ASCII passwords on the
       headless path stay intact. A \uXXXX yields one UTF-16 code unit directly; an
       astral character arrives as two \u escapes (a surrogate pair) and both units are
       stored, which is already valid UTF-16. */
    while (*p && *p != L'"') {
        wchar_t ch;
        if (*p == L'\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case L'"':  ch = L'"';  p++; break;
                case L'\\': ch = L'\\'; p++; break;
                case L'/':  ch = L'/';  p++; break;
                case L'b':  ch = L'\b'; p++; break;
                case L'f':  ch = L'\f'; p++; break;
                case L'n':  ch = L'\n'; p++; break;
                case L'r':  ch = L'\r'; p++; break;
                case L't':  ch = L'\t'; p++; break;
                case L'u': {
                    unsigned v = 0; int i;
                    p++;
                    for (i = 0; i < 4; i++) {
                        wchar_t h = *p;
                        if (h >= L'0' && h <= L'9') v = (v << 4) | (unsigned)(h - L'0');
                        else if (h >= L'a' && h <= L'f') v = (v << 4) | (unsigned)(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') v = (v << 4) | (unsigned)(h - L'A' + 10);
                        else { out[0] = 0; return FALSE; }
                        p++;
                    }
                    ch = (wchar_t)v;
                    break;
                }
                default: out[0] = 0; return FALSE;
            }
        } else {
            ch = *p;
            p++;
            if (ch < 0x20) { out[0] = 0; return FALSE; }
        }
        /* Embedded NUL would silently turn a decoded path into its prefix. */
        if (ch == 0) { out[0] = 0; return FALSE; }
        if (o + 1 >= out_len) { out[o] = 0; return FALSE; }
        out[o++] = ch;
    }
    if (*p != L'"') { out[0] = 0; return FALSE; }
    out[o] = 0;
    return TRUE;
}

BOOL json_get_int(const wchar_t *json, const wchar_t *key, int *out)
{
    const wchar_t *p = json_find_value(json, key);
    if (!p || !out) return FALSE;

    /* Handle quoted numbers from JS */
    if (*p == L'"') p++;
    *out = _wtoi(p);
    return TRUE;
}

BOOL json_get_bool(const wchar_t *json, const wchar_t *key, BOOL *out)
{
    const wchar_t *p = json_find_value(json, key);
    if (!p || !out) return FALSE;
    *out = (*p == L't' || *p == L'T') ? TRUE : FALSE;
    return TRUE;
}
