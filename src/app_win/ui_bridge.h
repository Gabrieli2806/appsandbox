#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

#include <windows.h>

/* Messages from the host to the app shell: one JSON object each, with a
   "type" (vmListChanged, hostInfo, fullState, log, alert, notice, ...; the
   list is in AGENTS.md section 7). The callback runs on the UI thread and
   must return quickly without dispatching messages; the text is only valid
   during the call. Messages posted before a callback is set are kept (up to
   256) and delivered when it is. */
typedef void (__stdcall *UiBridgeCallback)(const wchar_t *json);
void ui_bridge_set_callback(UiBridgeCallback cb);
void ui_bridge_post(const wchar_t *json);
void ui_bridge_cleanup(void);

/* ---- JSON builder helpers ---- */

typedef struct {
    wchar_t *buf;
    size_t   cap;
    size_t   len;
    int      count;  /* number of key-value pairs at current level */
    BOOL     truncated;  /* something did not fit: the text is not valid JSON */
} JsonBuilder;

void jb_init(JsonBuilder *jb, wchar_t *buf, size_t cap);
void jb_object_begin(JsonBuilder *jb);
void jb_object_end(JsonBuilder *jb);
void jb_array_begin(JsonBuilder *jb, const wchar_t *key);
void jb_array_end(JsonBuilder *jb);
void jb_string(JsonBuilder *jb, const wchar_t *key, const wchar_t *val);
void jb_int(JsonBuilder *jb, const wchar_t *key, int val);
void jb_bool(JsonBuilder *jb, const wchar_t *key, BOOL val);

/* Low-level append helpers (for building raw JSON arrays, etc.) */
void jb_append(JsonBuilder *jb, const wchar_t *s);
void jb_append_escaped(JsonBuilder *jb, const wchar_t *s);

/* ---- Simple JSON parser (for messages from JS) ---- */

BOOL json_has_key(const wchar_t *json, const wchar_t *key);

/* Extract a string value for a given key from a JSON string.
   Returns TRUE if found. out is null-terminated. */
BOOL json_get_string(const wchar_t *json, const wchar_t *key,
                     wchar_t *out, size_t out_len);

/* Extract an integer value for a given key. Returns TRUE if found. */
BOOL json_get_int(const wchar_t *json, const wchar_t *key, int *out);

/* Extract a boolean value for a given key. Returns TRUE if found. */
BOOL json_get_bool(const wchar_t *json, const wchar_t *key, BOOL *out);

#endif /* UI_BRIDGE_H */
