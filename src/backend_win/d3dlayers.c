/*
 * d3dlayers.c -- see d3dlayers.h.
 *
 * Pipeline (all on the host; mirrors tools/iso-patch/prefetch_repo.c idioms):
 *   1. GET displaycatalog.mp.microsoft.com for product 9NQPSL29BFFF
 *      -> parse the package's WuCategoryId (resolved live, never hardcoded).
 *   2. Windows Update FE3 client protocol over WinHTTP (HTTPS):
 *        GetCookie -> SyncUpdates(filtered by WuCategoryId)
 *                  -> GetExtendedUpdateInfo2 -> time-limited CDN url.
 *      The package for the build architecture is located by identifier in the
 *      response, so version changes / extra updates do not break resolution.
 *   3. Download the .appx (a zip) from the Microsoft CDN.
 *   4. Extract with the in-box tar.exe (libarchive handles zip natively).
 *   5. Verify, then copy the mapping-layer files into <exe_dir>\d3dlayers\.
 *
 * Verification. The CDN url is often plain HTTP and the DLLs end up in the
 * guest's System32, so a download is accepted only when either
 *   - the .appx matches the digest that FE3 returned over HTTPS for that file
 *     (SHA-256 from <AdditionalDigest> when present, else the SHA-1 Digest),
 *   - or every DLL carries a valid Authenticode signature from Microsoft.
 * Either proves the files come from Microsoft; two independent checks keep
 * the feature working if one of them stops applying (FE3 changes its reply,
 * or a package version ships DLLs without their own signature).
 * dzn_icd.json is not taken from the package: it is written from a fixed
 * template (only api_version is copied, digits and dots only), so the Vulkan
 * ICD path the guest registers can never point anywhere else.
 *
 * After a set is accepted, the SHA-256 of each cached file is written to
 * <exe_dir>\d3dlayers.verified (before the files are copied). A cache whose
 * files match that list is used without any further check or network access.
 * A cache that does not match is downloaded again; the old files stay until a
 * new set has been verified, so a failed or offline download loses nothing.
 * A cache without the list (made by an older version) is accepted once if
 * every DLL is signed by Microsoft, and the list is then written.
 *
 * When a downloaded set fails verification, <exe_dir>\d3dlayers.retry is
 * written and no new download is attempted for DL_RETRY_HOURS (deleting the
 * file allows an earlier retry), so a package that cannot be verified does
 * not cost a full download on every VM start.
 *
 * Defensive throughout: any failure returns FALSE and VM creation continues
 * without GL/CL/Vulkan acceleration.
 */

#include "d3dlayers.h"
#include "asb_paths.h"
#include "fe3_templates.h"
#include "ui.h"

#include <winhttp.h>
#include <rpc.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

#define DL_PRODUCT_ID       L"9NQPSL29BFFF"
#define DL_FE3_HOST         L"fe3.delivery.mp.microsoft.com"
#define DL_FE3_PATH         L"/ClientWebService/client.asmx"
#define DL_FE3_PATH_SECURED L"/ClientWebService/client.asmx/secured"
#define DL_SOAP_HEADERS     L"Content-Type: application/soap+xml; charset=utf-8\r\n"
#define DL_CACHE_SUBDIR     L"d3dlayers"
/* The Store package ships a per-architecture .appx (and arch subfolder inside);
   pick the one matching the guest = build architecture. */
#if defined(_M_ARM64)
#  define DL_ARCH        L"arm64"
#  define DL_ARCH_A       "arm64"
#else
#  define DL_ARCH        L"x64"
#  define DL_ARCH_A       "x64"
#endif
#define DL_APPX_NAME    L"D3DMappingLayers_" DL_ARCH L".appx"
#define DL_ISI_ARCH      "_" DL_ARCH_A "_"
#define DL_MAX_ROUNDS       5   /* FE3 faults intermittently; retry the chain */
#define DL_VERIFIED_FILE    L"d3dlayers.verified"   /* beside the cache dir */
#define DL_RETRY_FILE       L"d3dlayers.retry"      /* beside the cache dir */
#define DL_RETRY_HOURS      24
#define DL_JSON_NAME        L"dzn_icd.json"
/* Guest location of vulkan_dzn.dll, JSON-escaped. Must match the GL-layers
   share guest_path in gpu_enum.c (gpu_append_gl_layers_share). */
#define DL_DZN_GUEST_PATH   "C:\\\\Windows\\\\AppSandbox\\\\d3dlayers\\\\vulkan_dzn.dll"
#define DL_DZN_API_DEFAULT  "1.2.0"

/* Files to extract: dest leaf name <- source path inside the .appx. */
static const wchar_t *const DL_DST[] = {
    L"OpenGLOn12.dll", L"dxil.dll", L"OpenCLOn12.dll",
    L"clon12compiler.dll", L"vulkan_dzn.dll", L"dzn_icd.json"
};
static const wchar_t *const DL_SRC[] = {
    DL_ARCH L"\\OpenGLOn12.dll", DL_ARCH L"\\dxil.dll", DL_ARCH L"\\OpenCLOn12.dll",
    DL_ARCH L"\\clon12compiler.dll", DL_ARCH L"\\vulkan_dzn.dll", L"dzn_icd." DL_ARCH L".json"
};
#define DL_NFILES ((int)(sizeof(DL_DST)/sizeof(DL_DST[0])))

/* ---- small helpers (local; backend_win has no shared util header) ---- */

static void get_exe_dir(wchar_t *out, size_t cch)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, out, (DWORD)cch);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

static void mkdir_p(const wchar_t *path)
{
    wchar_t buf[MAX_PATH];
    wchar_t *p;
    wcsncpy_s(buf, MAX_PATH, path, _TRUNCATE);
    p = buf;
    if (wcslen(buf) >= 3 && buf[1] == L':' && buf[2] == L'\\') p = buf + 3;
    for (; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(buf, NULL); *p = L'\\'; }
    }
    CreateDirectoryW(buf, NULL);
}

static int run_hidden(const wchar_t *cmdline)
{
    wchar_t mut[2048];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD ec = 1;
    wcsncpy_s(mut, 2048, cmdline, _TRUNCATE);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        ui_log(L"d3dlayers: CreateProcess failed: %lu", GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

static BOOL file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* Fresh lowercase UUID (no braces) for the WS-Addressing MessageID. FE3
 * rejects replayed message IDs, so every request must use a new one. */
static void new_guid(char *out, size_t cch)
{
    UUID u;
    RPC_CSTR s = NULL;
    out[0] = 0;
    if (UuidCreate(&u) != RPC_S_OK && UuidCreateSequential(&u) != RPC_S_OK) return;
    if (UuidToStringA(&u, &s) == RPC_S_OK && s) {
        strncpy_s(out, cch, (const char *)s, _TRUNCATE);
        RpcStringFreeA(&s);
    }
}

/* WS-Security timestamps: created = now (UTC), expires = now + 5 minutes. */
static void iso_times(char *created, size_t ccch, char *expires, size_t ecch)
{
    SYSTEMTIME st, ex;
    FILETIME ft;
    ULARGE_INTEGER q;
    GetSystemTime(&st);
    sprintf_s(created, ccch, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    SystemTimeToFileTime(&st, &ft);
    q.LowPart = ft.dwLowDateTime; q.HighPart = ft.dwHighDateTime;
    q.QuadPart += (ULONGLONG)5 * 60 * 10000000ULL;
    ft.dwLowDateTime = q.LowPart; ft.dwHighDateTime = q.HighPart;
    FileTimeToSystemTime(&ft, &ex);
    sprintf_s(expires, ecch, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
              ex.wYear, ex.wMonth, ex.wDay, ex.wHour, ex.wMinute, ex.wSecond, ex.wMilliseconds);
}

/* Allocate a copy of src with every occurrence of tok replaced by val. */
static char *str_replace_all(const char *src, const char *tok, const char *val)
{
    size_t tl = strlen(tok), vl = strlen(val), cnt = 0;
    const char *p = src, *s;
    char *out, *d;
    while ((p = strstr(p, tok)) != NULL) { cnt++; p += tl; }
    out = (char *)malloc(strlen(src) + cnt * vl + 1);
    if (!out) return NULL;
    d = out; s = src;
    while (*s) {
        if (strncmp(s, tok, tl) == 0) { memcpy(d, val, vl); d += vl; s += tl; }
        else { *d++ = *s++; }
    }
    *d = 0;
    return out;
}

/* In-place minimal XML entity decode (FE3 escapes the inner update XML once). */
static void html_unescape(char *s)
{
    char *d = s, *p = s;
    while (*p) {
        if (*p == '&') {
            if (!strncmp(p, "&lt;", 4))        { *d++ = '<';  p += 4; continue; }
            if (!strncmp(p, "&gt;", 4))        { *d++ = '>';  p += 4; continue; }
            if (!strncmp(p, "&quot;", 6))      { *d++ = '"';  p += 6; continue; }
            if (!strncmp(p, "&apos;", 6))      { *d++ = '\''; p += 6; continue; }
            if (!strncmp(p, "&#39;", 5))       { *d++ = '\''; p += 5; continue; }
            if (!strncmp(p, "&amp;", 5))       { *d++ = '&';  p += 5; continue; }
        }
        *d++ = *p++;
    }
    *d = 0;
}

/* Copy text between a and b (after a, before next b) into out. */
static BOOL between(const char *buf, const char *a, const char *b, char *out, size_t cch)
{
    const char *p = strstr(buf, a), *q;
    size_t n;
    if (!p) return FALSE;
    p += strlen(a);
    q = strstr(p, b);
    if (!q) return FALSE;
    n = (size_t)(q - p);
    if (n >= cch) n = cch - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return TRUE;
}

/* Read an XML attribute value (attr="...") from a bounded tag string. */
static BOOL attr_in(const char *tag, const char *attr, char *out, size_t cch)
{
    char needle[64];
    sprintf_s(needle, sizeof(needle), "%s=\"", attr);
    return between(tag, needle, "\"", out, cch);
}

/* Last occurrence of needle in buf[0..limit). */
static const char *rfind(const char *buf, const char *limit, const char *needle)
{
    const char *best = NULL, *p = buf;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL && p < limit) { best = p; p += nl; }
    return best;
}

/* ---- WinHTTP ---- */

/* Perform a request; return the response body as a heap buffer (NUL-terminated)
 * regardless of HTTP status (SOAP faults carry a body we may inspect). Caller
 * frees. Returns NULL only on transport failure. */
static char *http_body(BOOL secure, const wchar_t *host, INTERNET_PORT port,
                       const wchar_t *verb, const wchar_t *path,
                       const wchar_t *headers, const void *body, DWORD body_len)
{
    HINTERNET hS = NULL, hC = NULL, hR = NULL;
    char *resp = NULL;
    DWORD cap = 0, used = 0;

    hS = WinHttpOpen(L"AppSandbox/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) goto done;
    hC = WinHttpConnect(hS, host, port, 0);
    if (!hC) goto done;
    hR = WinHttpOpenRequest(hC, verb, path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) goto done;
    if (!WinHttpSendRequest(hR, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                            headers ? (DWORD)-1L : 0,
                            (LPVOID)body, body_len, body_len, 0)) goto done;
    if (!WinHttpReceiveResponse(hR, NULL)) goto done;

    for (;;) {
        DWORD avail = 0, rd = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail)) break;
        if (avail == 0) break;
        if (used + avail + 1 > cap) {
            DWORD nc = (used + avail + 1) * 2;
            char *n = (char *)realloc(resp, nc);
            if (!n) { free(resp); resp = NULL; goto done; }
            resp = n; cap = nc;
        }
        if (!WinHttpReadData(hR, resp + used, avail, &rd)) break;
        if (rd == 0) break;
        used += rd;
    }
    if (resp) resp[used] = 0;

done:
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    return resp;
}

/* POST a SOAP envelope (UTF-8) to the FE3 service; returns the response body. */
static char *fe3_post(const wchar_t *path, const char *envelope)
{
    return http_body(TRUE, DL_FE3_HOST, INTERNET_DEFAULT_HTTPS_PORT, L"POST",
                     path, DL_SOAP_HEADERS, envelope, (DWORD)strlen(envelope));
}

/* GET a full URL and stream it to a file. Handles http/https and query string. */
static int http_download(const wchar_t *url, const wchar_t *out_path)
{
    URL_COMPONENTS uc;
    wchar_t host[256], upath[4096], extra[2048], fullpath[6144];
    HINTERNET hS = NULL, hC = NULL, hR = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    int rc = -1;
    BOOL secure;
    DWORD status = 0, slen = sizeof(status);
    BYTE buf[65536];

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath  = upath;   uc.dwUrlPathLength  = ARRAYSIZE(upath);
    uc.lpszExtraInfo = extra;  uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return -1;
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    swprintf_s(fullpath, ARRAYSIZE(fullpath), L"%s%s", upath, extra);

    hS = WinHttpOpen(L"AppSandbox/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) goto done;
    hC = WinHttpConnect(hS, host, uc.nPort, 0);
    if (!hC) goto done;
    hR = WinHttpOpenRequest(hC, L"GET", fullpath, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) goto done;
    if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(hR, NULL)) goto done;
    WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    if (status / 100 != 2) { ui_log(L"d3dlayers: download HTTP %lu", status); goto done; }

    hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) goto done;
    for (;;) {
        DWORD avail = 0, rd = 0, wr = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail)) goto done;
        if (avail == 0) break;
        if (avail > sizeof(buf)) avail = sizeof(buf);
        if (!WinHttpReadData(hR, buf, avail, &rd)) goto done;
        if (rd == 0) break;
        if (!WriteFile(hFile, buf, rd, &wr, NULL) || wr != rd) goto done;
    }
    rc = 0;

done:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    return rc;
}

/* ---- response parsing ---- */

/* Find the D3DMappingLayers .appx matching the build architecture in a
 * (decoded) SyncUpdates response and resolve its UpdateIdentity by joining the
 * file's enclosing <Update> <ID> to the matching NewUpdates <UpdateIdentity>.
 * digest receives the file's Digest attribute (base64 SHA-1) and sha256 the
 * base64 SHA-256 from its <AdditionalDigest Algorithm="SHA256"> child; either
 * is left empty when the reply does not carry it. */
static BOOL parse_leaf(const char *buf, char *uid, size_t uid_cch,
                           char *rev, size_t rev_cch, char *digest, size_t dig_cch,
                           char *sha256, size_t sha_cch)
{
    const char *p = buf;
    while ((p = strstr(p, "<File ")) != NULL) {
        const char *gt = strchr(p, '>');
        char tag[1024], fn[260], isi[260];
        const char *idp, *idend, *idtok_pos, *uip;
        char idtok[64], idnum[32];
        size_t taglen;
        if (!gt) break;
        taglen = (size_t)(gt - p);
        if (taglen >= sizeof(tag)) { p = gt + 1; continue; }
        memcpy(tag, p, taglen); tag[taglen] = 0;

        if (!attr_in(tag, "FileName", fn, sizeof(fn)) ||
            !attr_in(tag, "InstallerSpecificIdentifier", isi, sizeof(isi))) {
            p = gt + 1; continue;
        }
        if (!strstr(isi, "D3DMappingLayers") || !strstr(isi, DL_ISI_ARCH) ||
            !strstr(fn, ".appx")) {
            p = gt + 1; continue;
        }
        /* matched the package file for this architecture */
        digest[0] = 0;
        sha256[0] = 0;
        attr_in(tag, " Digest", digest, dig_cch);   /* not DigestAlgorithm */
        if (taglen == 0 || tag[taglen - 1] != '/') {
            static const char ad_tag[] = "<AdditionalDigest Algorithm=\"SHA256\">";
            const char *end = strstr(gt, "</File>");
            const char *ad = strstr(gt, ad_tag);
            if (end && ad && ad < end &&
                !between(ad, ad_tag, "<", sha256, sha_cch))
                sha256[0] = 0;
        }

        /* nearest <ID>N</ID> before this <File> = the ExtendedUpdateInfo id */
        idp = rfind(buf, p, "<ID>");
        if (!idp) return FALSE;
        idp += 4;
        idend = strstr(idp, "</ID>");
        if (!idend || (size_t)(idend - idp) >= sizeof(idnum)) return FALSE;
        memcpy(idnum, idp, (size_t)(idend - idp));
        idnum[idend - idp] = 0;

        /* first <ID>N</ID> in the buffer is the NewUpdates entry; the
         * UpdateIdentity follows it. */
        sprintf_s(idtok, sizeof(idtok), "<ID>%s</ID>", idnum);
        idtok_pos = strstr(buf, idtok);
        if (!idtok_pos) return FALSE;
        uip = strstr(idtok_pos, "<UpdateIdentity UpdateID=\"");
        if (!uip) return FALSE;
        if (!between(uip, "UpdateID=\"", "\"", uid, uid_cch)) return FALSE;
        if (!between(uip, "RevisionNumber=\"", "\"", rev, rev_cch))
            strcpy_s(rev, rev_cch, "1");
        return TRUE;
    }
    return FALSE;
}

/* From a (decoded) GetExtendedUpdateInfo2 response, pick the CDN url for the
 * file whose <FileDigest> matches `digest`; fall back to the first delivery
 * CDN url. */
static BOOL parse_file_url(const char *buf, const char *digest,
                           wchar_t *out, size_t out_cch)
{
    char urla[4096];
    const char *p;

    if (digest && digest[0]) {
        const char *d = strstr(buf, digest);
        if (d) {
            const char *u = strstr(d, "<Url>");
            if (u && between(u, "<Url>", "</Url>", urla, sizeof(urla))) {
                MultiByteToWideChar(CP_UTF8, 0, urla, -1, out, (int)out_cch);
                return TRUE;
            }
        }
    }
    /* fallback: first delivery.mp CDN url */
    p = buf;
    while ((p = strstr(p, "<Url>")) != NULL) {
        if (between(p, "<Url>", "</Url>", urla, sizeof(urla))) {
            if (strstr(urla, "delivery.mp.microsoft.com")) {
                MultiByteToWideChar(CP_UTF8, 0, urla, -1, out, (int)out_cch);
                return TRUE;
            }
        }
        p += 5;
    }
    return FALSE;
}

/* Resolve the (build-architecture) package CDN url from Microsoft, with the
 * package's base64 SHA-1 and SHA-256 digests (either may come back empty).
 * Returns 0 on success. */
static int resolve_url(const char *wucatid, wchar_t *url_out, size_t url_cch,
                       char *sha1_out, size_t sha1_cch,
                       char *sha256_out, size_t sha256_cch)
{
    int round, rc = -1;
    for (round = 0; round < DL_MAX_ROUNDS && rc != 0; round++) {
        char msgid[40], created[40], expires[40];
        char *t1, *cookie_req = NULL, *cookie_resp = NULL;
        char cookie[8192];
        char *sync_req = NULL, *sync_resp = NULL;
        char uid[128] = {0}, rev[32] = {0}, digest[256] = {0}, sha256[256] = {0};
        char *ext_req = NULL, *ext_resp = NULL;

        new_guid(msgid, sizeof(msgid));
        iso_times(created, sizeof(created), expires, sizeof(expires));

        /* GetCookie */
        t1 = str_replace_all(FE3_TPL_GETCOOKIE, "%MSGID%", msgid);
        if (t1) { cookie_req = str_replace_all(t1, "%CREATED%", created); free(t1); }
        if (cookie_req) { t1 = str_replace_all(cookie_req, "%EXPIRES%", expires); free(cookie_req); cookie_req = t1; }
        if (cookie_req) { cookie_resp = fe3_post(DL_FE3_PATH, cookie_req); free(cookie_req); }
        if (!cookie_resp || !between(cookie_resp, "EncryptedData>", "</", cookie, sizeof(cookie))) {
            free(cookie_resp); Sleep(1500); continue;
        }
        free(cookie_resp);

        /* SyncUpdates */
        new_guid(msgid, sizeof(msgid));
        t1 = str_replace_all(FE3_TPL_SYNCUPDATES, "%MSGID%", msgid);
        if (t1) { sync_req = str_replace_all(t1, "%CREATED%", created); free(t1); t1 = sync_req; }
        if (t1) { sync_req = str_replace_all(t1, "%EXPIRES%", expires); free(t1); t1 = sync_req; }
        if (t1) { sync_req = str_replace_all(t1, "%COOKIE%", cookie); free(t1); t1 = sync_req; }
        if (t1) { sync_req = str_replace_all(t1, "%CATID%", wucatid); free(t1); }
        if (sync_req) { sync_resp = fe3_post(DL_FE3_PATH, sync_req); free(sync_req); }
        if (sync_resp) html_unescape(sync_resp);
        if (!sync_resp || !parse_leaf(sync_resp, uid, sizeof(uid), rev, sizeof(rev),
                                      digest, sizeof(digest), sha256, sizeof(sha256))) {
            free(sync_resp); Sleep(1500); continue;
        }
        free(sync_resp);

        /* GetExtendedUpdateInfo2 */
        new_guid(msgid, sizeof(msgid));
        t1 = str_replace_all(FE3_TPL_GETEXTENDEDINFO2, "%MSGID%", msgid);
        if (t1) { ext_req = str_replace_all(t1, "%CREATED%", created); free(t1); t1 = ext_req; }
        if (t1) { ext_req = str_replace_all(t1, "%EXPIRES%", expires); free(t1); t1 = ext_req; }
        if (t1) { ext_req = str_replace_all(t1, "%UPDATEID%", uid); free(t1); t1 = ext_req; }
        if (t1) { ext_req = str_replace_all(t1, "%REVISION%", rev); free(t1); }
        if (ext_req) { ext_resp = fe3_post(DL_FE3_PATH_SECURED, ext_req); free(ext_req); }
        if (ext_resp) html_unescape(ext_resp);
        if (ext_resp && parse_file_url(ext_resp, digest, url_out, url_cch)) {
            strncpy_s(sha1_out, sha1_cch, digest, _TRUNCATE);
            strncpy_s(sha256_out, sha256_cch, sha256, _TRUNCATE);
            rc = 0;
        } else
            Sleep(1500);
        free(ext_resp);
    }
    return rc;
}

/* ---- Vulkan ICD manifest ---- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Read api_version from a dzn_icd JSON file into out. Only a value made of
 * digits and dots (starting with a digit) is taken; anything else, or a
 * missing file, gives DL_DZN_API_DEFAULT. */
static void read_dzn_api_version(const wchar_t *path, char *out, size_t cch)
{
    FILE *f = NULL;
    char buf[4096];
    size_t rd, n;
    const char *p;

    strcpy_s(out, cch, DL_DZN_API_DEFAULT);
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = 0;

    p = strstr(buf, "\"api_version\"");
    if (!p) return;
    p = skip_ws(p + 13);
    if (*p != ':') return;
    p = skip_ws(p + 1);
    if (*p != '"') return;
    p++;
    if (*p < '0' || *p > '9') return;
    for (n = 0; (p[n] >= '0' && p[n] <= '9') || p[n] == '.'; n++)
        ;
    if (p[n] != '"' || n >= cch) return;
    memcpy(out, p, n);
    out[n] = 0;
}

/* Write the Vulkan ICD manifest to dst_path from a fixed template. Nothing in
 * it comes from the download except api_version (read from src_path, which
 * may be the same file), so library_path is always the expected DLL.
 *
 * library_path is the DLL's fully-qualified location in the guest: the layers
 * are staged into one flat directory, and the Vulkan loader resolves an ICD
 * via LoadLibraryW(), falling back to LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_*),
 * where LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires a fully-qualified path. */
static BOOL write_dzn_json(const wchar_t *src_path, const wchar_t *dst_path)
{
    char api[32], json[512];
    FILE *f = NULL;
    int len;
    BOOL ok;

    read_dzn_api_version(src_path, api, sizeof(api));
    len = _snprintf_s(json, sizeof(json), _TRUNCATE,
        "{\n"
        "    \"file_format_version\": \"1.0.0\",\n"
        "    \"ICD\": {\n"
        "        \"library_path\": \"" DL_DZN_GUEST_PATH "\",\n"
        "        \"api_version\": \"%s\"\n"
        "    }\n"
        "}\n", api);
    if (len <= 0) return FALSE;
    if (_wfopen_s(&f, dst_path, L"wb") != 0 || !f) {
        ui_log(L"d3dlayers: cannot write %s", dst_path);
        return FALSE;
    }
    ok = fwrite(json, 1, (size_t)len, f) == (size_t)len;
    if (fclose(f) != 0) ok = FALSE;
    return ok;
}

/* ---- hashes ---- */

/* Hash a file with a BCrypt algorithm (BCRYPT_SHA1_ALGORITHM or
 * BCRYPT_SHA256_ALGORITHM). Returns the digest length, 0 on failure. */
static ULONG file_hash(const wchar_t *path, LPCWSTR alg_name, BYTE *out, ULONG out_cb)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE h;
    BYTE buf[65536];
    DWORD got;
    ULONG len = 0, cb = 0, result = 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, alg_name, NULL, 0) != 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof(len), &cb, 0) != 0 ||
        len == 0 || len > out_cb) goto done;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) != 0) goto done;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &got, NULL)) goto done;
        if (got == 0) break;
        if (BCryptHashData(hash, buf, got, 0) != 0) goto done;
    }
    if (BCryptFinishHash(hash, out, len, 0) != 0) goto done;
    result = len;

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return result;
}

/* TRUE when the file's hash (alg_name) equals the base64 digest b64. */
static BOOL file_matches_b64(const wchar_t *path, LPCWSTR alg_name, const char *b64)
{
    BYTE want[64], have[64];
    DWORD want_len = sizeof(want);
    ULONG have_len;

    if (!b64 || !b64[0]) return FALSE;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, want, &want_len, NULL, NULL))
        return FALSE;
    have_len = file_hash(path, alg_name, have, sizeof(have));
    return have_len != 0 && have_len == want_len && memcmp(want, have, have_len) == 0;
}

/* TRUE when the downloaded package matches a digest FE3 returned for it over
 * HTTPS (SHA-256 or SHA-1, base64; an empty one is skipped). */
static BOOL package_matches_digest(const wchar_t *appx, const char *sha1_b64,
                                   const char *sha256_b64)
{
    if (file_matches_b64(appx, BCRYPT_SHA256_ALGORITHM, sha256_b64)) {
        ui_log(L"d3dlayers: package matches the SHA-256 digest from Windows Update");
        return TRUE;
    }
    if (file_matches_b64(appx, BCRYPT_SHA1_ALGORITHM, sha1_b64)) {
        ui_log(L"d3dlayers: package matches the SHA-1 digest from Windows Update");
        return TRUE;
    }
    if (sha256_b64[0] || sha1_b64[0])
        ui_log(L"d3dlayers: package does not match the digest from Windows Update; "
               L"checking the DLL signatures instead");
    else
        ui_log(L"d3dlayers: Windows Update gave no digest for the package; "
               L"checking the DLL signatures instead");
    return FALSE;
}

/* ---- signature check ---- */

/* TRUE when path has a valid embedded Authenticode signature whose signer
   is Microsoft ("Microsoft Corporation" or "Microsoft Windows"), the same
   rule the guest agent applies before it replaces opengl32.dll. No
   revocation check. URL retrieval stays allowed so that Windows can fetch
   a Microsoft root it does not have yet (automatic root update); with the
   roots present the check needs no network. */
static BOOL signed_by_microsoft(const wchar_t *path)
{
    static const GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO file;
    WINTRUST_DATA trust;
    CRYPT_PROVIDER_DATA *provider;
    CRYPT_PROVIDER_SGNR *signer;
    wchar_t name[256] = L"";
    LONG status;
    BOOL ok = FALSE;

    ZeroMemory(&file, sizeof(file));
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path;
    ZeroMemory(&trust, sizeof(trust));
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    status = WinVerifyTrust(NULL, (GUID *)&policy, &trust);
    if (status == ERROR_SUCCESS) {
        provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : NULL;
        if (signer && signer->csCertChain && signer->pasCertChain &&
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               0, NULL, name, ARRAYSIZE(name)))
            ok = !_wcsicmp(name, L"Microsoft Corporation") || !_wcsicmp(name, L"Microsoft Windows");
        if (!ok)
            ui_log(L"d3dlayers: %s is signed by \"%s\", not Microsoft", path, name);
    } else {
        ui_log(L"d3dlayers: %s has no valid signature (0x%08lX)", path, (unsigned long)status);
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, (GUID *)&policy, &trust);
    return ok;
}

static BOOL is_dll(const wchar_t *name)
{
    size_t n = wcslen(name);
    return n > 4 && _wcsicmp(name + n - 4, L".dll") == 0;
}

/* TRUE when every DLL of the set in dir is signed by Microsoft. */
static BOOL layers_signed(const wchar_t *dir)
{
    int i;
    for (i = 0; i < DL_NFILES; i++) {
        wchar_t f[MAX_PATH];
        if (!is_dll(DL_DST[i])) continue;      /* dzn_icd.json: written by us */
        swprintf_s(f, MAX_PATH, L"%s\\%s", dir, DL_DST[i]);
        if (!signed_by_microsoft(f)) return FALSE;
    }
    return TRUE;
}

/* ---- cache bookkeeping ---- */

/* TRUE when every file of the set exists in dir. */
static BOOL layers_present(const wchar_t *dir)
{
    int i;
    for (i = 0; i < DL_NFILES; i++) {
        wchar_t f[MAX_PATH];
        swprintf_s(f, MAX_PATH, L"%s\\%s", dir, DL_DST[i]);
        if (!file_exists(f)) return FALSE;
    }
    return TRUE;
}

/* The verified list for the set in dir: one line "<sha256 hex> <name>" per
 * file. FALSE when a file cannot be read. */
static BOOL build_verified_list(const wchar_t *dir, char *out, size_t cch)
{
    static const char hexdig[] = "0123456789abcdef";
    size_t used = 0;
    int i;

    out[0] = 0;
    for (i = 0; i < DL_NFILES; i++) {
        wchar_t f[MAX_PATH];
        BYTE h[32];
        char hex[65];
        ULONG j;
        int n;
        swprintf_s(f, MAX_PATH, L"%s\\%s", dir, DL_DST[i]);
        if (file_hash(f, BCRYPT_SHA256_ALGORITHM, h, sizeof(h)) != sizeof(h)) return FALSE;
        for (j = 0; j < sizeof(h); j++) {
            hex[j * 2]     = hexdig[h[j] >> 4];
            hex[j * 2 + 1] = hexdig[h[j] & 15];
        }
        hex[64] = 0;
        n = _snprintf_s(out + used, cch - used, _TRUNCATE, "%s %ls\r\n", hex, DL_DST[i]);
        if (n < 0) return FALSE;
        used += (size_t)n;
    }
    return TRUE;
}

static BOOL write_text(const wchar_t *path, const char *text)
{
    FILE *f = NULL;
    size_t len = strlen(text);
    BOOL ok;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
        ui_log(L"d3dlayers: cannot write %s", path);
        return FALSE;
    }
    ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = FALSE;
    return ok;
}

/* 1 when the files in dir match the verified list at list_path, 0 when they
 * do not (or cannot be read), -1 when there is no list. */
static int cache_matches_list(const wchar_t *dir, const wchar_t *list_path)
{
    char want[1024], have[1024];
    FILE *f = NULL;
    size_t rd;

    if (!file_exists(list_path)) return -1;
    if (_wfopen_s(&f, list_path, L"rb") != 0 || !f) return 0;
    rd = fread(want, 1, sizeof(want) - 1, f);
    fclose(f);
    want[rd] = 0;
    if (!build_verified_list(dir, have, sizeof(have))) return 0;
    return strcmp(want, have) == 0 ? 1 : 0;
}

/* TRUE while the retry marker is younger than DL_RETRY_HOURS. A marker dated
 * in the future (clock moved back) does not block. */
static BOOL retry_blocked(const wchar_t *marker)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILETIME now_ft;
    ULARGE_INTEGER t, now;

    if (!GetFileAttributesExW(marker, GetFileExInfoStandard, &fad)) return FALSE;
    GetSystemTimeAsFileTime(&now_ft);
    t.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    now.LowPart = now_ft.dwLowDateTime;
    now.HighPart = now_ft.dwHighDateTime;
    if (t.QuadPart > now.QuadPart) return FALSE;
    return now.QuadPart - t.QuadPart < (ULONGLONG)DL_RETRY_HOURS * 3600ULL * 10000000ULL;
}

static void retry_mark(const wchar_t *marker)
{
    SYSTEMTIME st;
    char text[256];
    GetSystemTime(&st);
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "The D3D mapping layers downloaded at %04u-%02u-%02u %02u:%02u UTC failed "
                "verification. App Sandbox downloads them again %d hours after that; "
                "delete this file to retry sooner.\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, DL_RETRY_HOURS);
    write_text(marker, text);
}

static void remove_tree(const wchar_t *dir)
{
    wchar_t cmd[2048];
    swprintf_s(cmd, ARRAYSIZE(cmd), L"cmd.exe /c rd /s /q \"%s\" 2>nul", dir);
    run_hidden(cmd);
}

/* ---- public API ---- */

BOOL d3dlayers_ensure_cached(wchar_t *out_dir, int out_max)
{
    wchar_t base[MAX_PATH], cache[MAX_PATH], tmp[MAX_PATH], stage[MAX_PATH];
    wchar_t appx[MAX_PATH], list_path[MAX_PATH], retry_path[MAX_PATH];
    wchar_t sysdir[MAX_PATH], cmd[2048];
    char *catalog = NULL, wucatid[64] = {0};
    char sha1[256] = {0}, sha256[256] = {0};
    char list[1024];
    wchar_t url[6144];
    int i, state;
    BOOL verified;

    asb_writable_dir(base, MAX_PATH);   /* beside the exe, or ProgramData for MSIX */
    swprintf_s(cache, MAX_PATH, L"%s\\%s", base, DL_CACHE_SUBDIR);
    swprintf_s(list_path, MAX_PATH, L"%s\\%s", base, DL_VERIFIED_FILE);
    swprintf_s(retry_path, MAX_PATH, L"%s\\%s", base, DL_RETRY_FILE);

    /* Already cached? */
    if (layers_present(cache)) {
        state = cache_matches_list(cache, list_path);
        if (state == 1) {
            wcsncpy_s(out_dir, out_max, cache, _TRUNCATE);
            ui_log(L"d3dlayers: using cached layers at %s", cache);
            return TRUE;
        }
        if (state < 0) {
            /* A cache made before the verified list existed: accept it if
               every DLL is signed by Microsoft, with a fixed ICD manifest. */
            wchar_t json[MAX_PATH];
            swprintf_s(json, MAX_PATH, L"%s\\%s", cache, DL_JSON_NAME);
            if (layers_signed(cache) && write_dzn_json(json, json) &&
                build_verified_list(cache, list, sizeof(list)) &&
                write_text(list_path, list)) {
                wcsncpy_s(out_dir, out_max, cache, _TRUNCATE);
                ui_log(L"d3dlayers: verified the cached layers at %s", cache);
                return TRUE;
            }
            ui_log(L"d3dlayers: the cached layers could not be verified; downloading them again");
        } else {
            ui_log(L"d3dlayers: the cached layers differ from the verified copy; downloading them again");
        }
    }

    if (retry_blocked(retry_path)) {
        ui_log(L"d3dlayers: the last download failed verification, so the layers are downloaded "
               L"again only %d hours after it (delete %s to retry now); "
               L"GL/CL/Vulkan acceleration is off for this VM", DL_RETRY_HOURS, retry_path);
        return FALSE;
    }
    mkdir_p(cache);

    /* 1. WuCategoryId from the Microsoft display catalog. */
    catalog = http_body(TRUE, L"displaycatalog.mp.microsoft.com",
                        INTERNET_DEFAULT_HTTPS_PORT, L"GET",
                        L"/v7.0/products/" DL_PRODUCT_ID
                        L"?languages=en-us&market=US&fieldsTemplate=Details",
                        NULL, NULL, 0);
    if (!catalog || !between(catalog, "\"WuCategoryId\":\"", "\"",
                             wucatid, sizeof(wucatid))) {
        ui_log(L"d3dlayers: could not resolve WuCategoryId from displaycatalog");
        free(catalog);
        return FALSE;
    }
    free(catalog);
    {
        wchar_t w[64];
        MultiByteToWideChar(CP_UTF8, 0, wucatid, -1, w, 64);
        ui_log(L"d3dlayers: WuCategoryId=%s", w);
    }

    /* 2. FE3 chain -> CDN url and the package digests. */
    if (resolve_url(wucatid, url, ARRAYSIZE(url), sha1, sizeof(sha1),
                    sha256, sizeof(sha256)) != 0) {
        ui_log(L"d3dlayers: FE3 did not return a download url (after %d rounds)", DL_MAX_ROUNDS);
        return FALSE;
    }

    /* 3. Download the .appx. */
    swprintf_s(appx, MAX_PATH, L"%s\\%s", cache, DL_APPX_NAME);
    ui_log(L"d3dlayers: downloading D3DMappingLayers package...");
    if (http_download(url, appx) != 0) {
        ui_log(L"d3dlayers: package download failed");
        DeleteFileW(appx);
        return FALSE;
    }

    /* 4. Extract with in-box tar.exe (libarchive reads zip/.appx). */
    swprintf_s(tmp, MAX_PATH, L"%s\\_extract", cache);
    swprintf_s(stage, MAX_PATH, L"%s\\_stage", cache);
    remove_tree(tmp);
    remove_tree(stage);
    mkdir_p(tmp);
    mkdir_p(stage);
    GetSystemDirectoryW(sysdir, MAX_PATH);
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", sysdir, appx, tmp);
    if (run_hidden(cmd) != 0) {
        ui_log(L"d3dlayers: tar extraction failed");
        remove_tree(tmp);
        remove_tree(stage);
        DeleteFileW(appx);
        return FALSE;
    }

    /* 5. Collect the set flat in the staging folder; the ICD manifest is
       written from the template, not copied. */
    for (i = 0; i < DL_NFILES; i++) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        swprintf_s(src, MAX_PATH, L"%s\\%s", tmp, DL_SRC[i]);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", stage, DL_DST[i]);
        if (!is_dll(DL_DST[i])) {
            if (!file_exists(src))
                ui_log(L"d3dlayers: the package has no %s", DL_SRC[i]);
            else
                write_dzn_json(src, dst);
        } else if (!CopyFileW(src, dst, FALSE)) {
            ui_log(L"d3dlayers: copy %s failed: %lu", DL_DST[i], GetLastError());
        }
    }

    verified = layers_present(stage) &&
               (package_matches_digest(appx, sha1, sha256) || layers_signed(stage));
    remove_tree(tmp);
    DeleteFileW(appx);

    if (!layers_present(stage)) {
        ui_log(L"d3dlayers: the package does not contain every layer file");
        remove_tree(stage);
        return FALSE;
    }
    if (!verified) {
        retry_mark(retry_path);
        remove_tree(stage);
        ui_log(L"d3dlayers: the downloaded layers failed verification and were discarded; "
               L"GL/CL/Vulkan acceleration is off for this VM");
        return FALSE;
    }

    /* The list goes first: if the copy below stops part-way, the cache no
       longer matches it and is downloaded again instead of being used. */
    if (!build_verified_list(stage, list, sizeof(list)) || !write_text(list_path, list)) {
        remove_tree(stage);
        return FALSE;
    }
    for (i = 0; i < DL_NFILES; i++) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        swprintf_s(src, MAX_PATH, L"%s\\%s", stage, DL_DST[i]);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", cache, DL_DST[i]);
        SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(src, dst, FALSE))
            ui_log(L"d3dlayers: copy %s failed: %lu", DL_DST[i], GetLastError());
    }
    remove_tree(stage);
    if (cache_matches_list(cache, list_path) != 1) {
        ui_log(L"d3dlayers: could not replace the cached layers in %s", cache);
        return FALSE;
    }
    DeleteFileW(retry_path);

    wcsncpy_s(out_dir, out_max, cache, _TRUNCATE);
    ui_log(L"d3dlayers: staged %d mapping-layer files to %s", DL_NFILES, cache);
    return TRUE;
}
