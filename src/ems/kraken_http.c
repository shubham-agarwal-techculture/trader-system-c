#include "trading/kraken_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <winhttp.h>
#else
/* Non-Windows stub: no transport available. */
char *kraken_http_get(const char *p, int *s) { (void)p; if (s) *s = 0; return NULL; }
char *kraken_http_post(const char *p, const char *d, const char *h, int *s)
{ (void)p;(void)d;(void)h; if (s) *s = 0; return NULL; }
#endif

#if defined(_WIN32)

#define KRAKEN_HOST L"api.kraken.com"
#define KRAKEN_PORT INTERNET_DEFAULT_HTTPS_PORT
#define KRAKEN_BASE_PATH L"/0"

static char *http_request(const wchar_t *method, const char *path,
                          const char *post_data, const char *extra_headers,
                          int *http_status)
{
    *http_status = 0;
    HINTERNET session = WinHttpOpen(L"trading_system_c/0.1",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return NULL;
    /* Timeouts: resolve/connect/send/receive 10s each. */
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

    wchar_t wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);

    HINTERNET connect = WinHttpConnect(session, KRAKEN_HOST, KRAKEN_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return NULL; }

    DWORD flags = WINHTTP_FLAG_SECURE;
    HINTERNET request = WinHttpOpenRequest(connect, method, wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return NULL;
    }

    const wchar_t *headers =
        L"Content-Type: application/x-www-form-urlencoded\r\n";
    DWORD header_len = 0;
    LPWSTR all_headers = NULL;
    wchar_t extra_w[2048];
    if (extra_headers) {
        MultiByteToWideChar(CP_UTF8, 0, extra_headers, -1, extra_w, 2048);
        size_t hl = wcslen(headers) + wcslen(extra_w) + 4;
        all_headers = malloc(hl * sizeof(wchar_t));
        swprintf(all_headers, hl, L"%s%s", headers, extra_w);
        header_len = (DWORD)(wcslen(all_headers) * sizeof(wchar_t));
        headers = all_headers;
    } else {
        header_len = (DWORD)(wcslen(headers) * sizeof(wchar_t));
    }

    BOOL ok = WinHttpSendRequest(request, headers, header_len,
        (LPVOID)post_data,
        post_data ? (DWORD)strlen(post_data) : 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, NULL);
    }
    if (ok) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
            WINHTTP_NO_HEADER_INDEX);
        *http_status = (int)status;
    }

    char *body = NULL;
    size_t body_len = 0, body_cap = 0;
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) break;
            if (avail == 0) break;
            if (body_len + avail + 1 > body_cap) {
                body_cap = (body_len + avail + 1) * 2;
                body = realloc(body, body_cap);
                if (!body) break;
            }
            DWORD read = 0;
            if (!WinHttpReadData(request, body + body_len, avail, &read)) break;
            body_len += read;
            if (read == 0) break;
        }
    }
    if (body) body[body_len] = '\0';

    free(all_headers);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

char *kraken_http_get(const char *path_with_query, int *http_status)
{
    char full[1200];
    snprintf(full, sizeof(full), "/0%s", path_with_query);
    return http_request(L"GET", full, NULL, NULL, http_status);
}

char *kraken_http_post(const char *path, const char *post_data,
                       const char *extra_headers, int *http_status)
{
    char full[1200];
    snprintf(full, sizeof(full), "/0%s", path);
    return http_request(L"POST", full, post_data, extra_headers,
                        http_status);
}

#endif /* _WIN32 */
