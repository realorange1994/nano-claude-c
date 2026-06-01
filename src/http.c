#ifdef _MSC_VER
#pragma warning(disable:4819)
#endif
#include "http.h"
#include "buffer.h"
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

void http_init(void) {
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_ALL);
#endif
}

void http_cleanup(void) {
#ifndef _WIN32
    curl_global_cleanup();
#endif
}

#ifdef _WIN32
// --- Windows: WinHTTP implementation -------------------------------------------------------

#else
// --- Linux/macOS: libcurl helpers -----------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MemBuf;

static void mb_init(MemBuf *mb) {
    mb->data = NULL; mb->len = 0; mb->cap = 0;
}

static void mb_append(MemBuf *mb, const char *src, size_t n) {
    if (mb->len + n + 1 > mb->cap) {
        size_t new_cap = (mb->len + n + 1) * 2;
        char *p = realloc(mb->data, new_cap);
        if (!p) return;
        mb->data = p;
        mb->cap = new_cap;
    }
    memcpy(mb->data + mb->len, src, n);
    mb->len += n;
    mb->data[mb->len] = '\0';
}

static void mb_free(MemBuf *mb) {
    free(mb->data);
    mb->data = NULL; mb->len = 0; mb->cap = 0;
}

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *mb = (MemBuf *)userdata;
    size_t n = size * nmemb;
    mb_append(mb, (const char *)ptr, n);
    return n;
}

typedef struct {
    StreamCallback cb;
    void *userdata;
    char *buf;
    size_t buf_len;
    size_t buf_cap;
    int *cancelled;
    int ok;
} SSECtx;

static void sse_ctx_init(SSECtx *ctx, StreamCallback cb, void *userdata,
                         const volatile long *cancelled) {
    ctx->cb = cb;
    ctx->userdata = userdata;
    ctx->buf = NULL;
    ctx->buf_len = 0;
    ctx->buf_cap = 0;
    ctx->cancelled = (int *)cancelled;
    ctx->ok = 1;
}

static void sse_flush(SSECtx *ctx) {
    if (ctx->buf && ctx->buf_len > 0) {
        // dispatch only data: lines
        char *ptr = ctx->buf;
        char *eol;
        while ((eol = strchr(ptr, '\n')) != NULL) {
            *eol = '\0';
            size_t linelen = (size_t)(eol - ptr);
            if (linelen > 0 && ptr[linelen - 1] == '\r') {
                linelen--;
                ptr[linelen] = '\0';
            }
            if (strncmp(ptr, "data: ", 6) == 0) {
                ctx->cb(ptr + 6, ctx->userdata);
            }
            ptr = eol + 1;
        }
        // shift remainder
        size_t rem = ctx->buf_len - (size_t)(ptr - ctx->buf);
        if (rem > 0) memmove(ctx->buf, ptr, rem);
        ctx->buf_len = rem;
        ctx->buf[ctx->buf_len] = '\0';
    }
}

static size_t curl_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    SSECtx *ctx = (SSECtx *)userdata;
    if (!ctx || !ctx->ok) return 0;
    if (ctx->cancelled && *ctx->cancelled) {
        ctx->ok = 0;
        return 0;
    }
    size_t n = size * nmemb;
    size_t new_len = ctx->buf_len + n;
    if (new_len + 1 > ctx->buf_cap) {
        size_t new_cap = (new_len + 1) * 2;
        char *new_buf = realloc(ctx->buf, new_cap);
        if (!new_buf) { ctx->ok = 0; return 0; }
        ctx->buf = new_buf;
        ctx->buf_cap = new_cap;
    }
    memcpy(ctx->buf + ctx->buf_len, ptr, n);
    ctx->buf_len += n;
    ctx->buf[ctx->buf_len] = '\0';

    sse_flush(ctx);
    return n;
}

static void sse_ctx_free(SSECtx *ctx) {
    if (ctx->buf && ctx->buf_len > 0) {
        // final flush -- dispatch any remaining data: line
        if (strncmp(ctx->buf, "data: ", 6) == 0) {
            ctx->cb(ctx->buf + 6, ctx->userdata);
        }
    }
    free(ctx->buf);
}

static void parse_url(const char *url, char *scheme, char *host, int *port, char *path) {
    const char *slash_slash = strstr(url, "://");
    if (!slash_slash) { strcpy(path, "/"); return; }

    size_t scheme_len = (size_t)(slash_slash - url);
    if (scheme_len > 15) scheme_len = 15;
    strncpy(scheme, url, scheme_len);
    scheme[scheme_len] = '\0';

    const char *host_start = slash_slash + 3;
    const char *path_start = strchr(host_start, '/');
    const char *colon = strchr(host_start, ':');

    int default_port = (strcmp(scheme, "https") == 0) ? 443 : 80;
    *port = default_port;

    if (path_start) {
        size_t host_len = (size_t)(path_start - host_start);
        if (colon && colon < path_start) {
            host_len = (size_t)(colon - host_start);
            *port = atoi(colon + 1);
            if (*port == 0) *port = default_port;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(path, path_start, 1023);
        path[1023] = '\0';
    } else {
        size_t host_len = strlen(host_start);
        if (colon) {
            host_len = (size_t)(colon - host_start);
            *port = atoi(colon + 1);
            if (*port == 0) *port = default_port;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        strcpy(path, "/");
    }
}
#endif // _WIN32

// --- http_post -----------------------------------------------------------------------------

char *http_post(const char *url, const char *headers_str, const char *body, int timeout_ms) {
    if (!url || !body) return NULL;

#ifdef _WIN32
    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};

    if (sscanf(url, "%15[^://]://%255[^:/]:%d/%1023[^\n]", scheme, host, &port, path) != 4) {
        if (sscanf(url, "%15[^://]://%255[^/\n]", scheme, host) >= 2) {
            port = 443;
            strcpy(path, "/");
        } else {
            fprintf(stderr, "[DEBUG][http_post] URL parsing failed: %s\n", url);
            return NULL;
        }
    }

    int use_ssl = (strcmp(scheme, "https") == 0) ? 1 : 0;
    if (port == 0) port = use_ssl ? 443 : 80;

    fprintf(stderr, "[DEBUG][http_post] Connecting to %s:%d (%s)\n", host, port, scheme);
    fflush(stderr);

    wchar_t *w_host = utf8_to_wide(host);
    wchar_t *w_path = utf8_to_wide(path);

    HINTERNET hSession = WinHttpOpen(L"NanoClaude/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { free(w_host); free(w_path); return NULL; }

    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, w_host, (INTERNET_PORT)port, 0);
    free(w_host);
    if (!hConnect) { WinHttpCloseHandle(hSession); free(w_path); return NULL; }

    DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", w_path, NULL, NULL, NULL, flags);
    free(w_path);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return NULL; }

    if (headers_str) {
        wchar_t *w_headers = utf8_to_wide(headers_str);
        if (w_headers) {
            WinHttpAddRequestHeaders(hRequest, w_headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            free(w_headers);
        }
    }

    if (use_ssl) {
        DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
    }

    BOOL result = WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);
    if (!result) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return NULL;
    }

    WinHttpReceiveResponse(hRequest, NULL);

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status_code, &size, NULL);

    if (status_code != 200) {
        Buffer buf; buffer_init(&buf);
        char chunk[4096]; DWORD bytes_read;
        while (WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &bytes_read) && bytes_read > 0) {
            chunk[bytes_read] = '\0';
            buffer_append(&buf, chunk, bytes_read);
        }
        fprintf(stderr, "[DEBUG][http_post] Error response: %s\n", buffer_c_str(&buf));
        buffer_free(&buf);
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return NULL;
    }

    Buffer buf; buffer_init(&buf);
    char chunk[4096]; DWORD bytes_read;
    while (WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &bytes_read) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        buffer_append(&buf, chunk, bytes_read);
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);

    char *response = buffer_steal(&buf);
    if (!response) response = strdup("");
    buffer_free(&buf);
    return response;

#else
    // Linux/macOS: libcurl
    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};
    parse_url(url, scheme, host, &port, path);

    char full_url[2048];
    snprintf(full_url, sizeof(full_url), "%s://%s:%d%s", scheme, host, port, path);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct curl_slist *hdrs = NULL;
    if (headers_str) {
        // headers_str is multiline, split by \n
        char tmp[4096];
        strncpy(tmp, headers_str, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *save = NULL;
        char *line = strtok_r(tmp, "\n", &save);
        while (line) {
            if (*line) hdrs = curl_slist_append(hdrs, line);
            line = strtok_r(NULL, "\n", &save);
        }
    }

    MemBuf resp;
    mb_init(&resp);

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        if (res != CURLE_OK)
            fprintf(stderr, "[DEBUG][http_post] curl error: %s\n", curl_easy_strerror(res));
        mb_free(&resp);
        return NULL;
    }

    char *result = strdup(resp.data ? resp.data : "");
    mb_free(&resp);
    return result;
#endif
}

// --- http_post_stream ----------------------------------------------------------------------

bool http_post_stream(const char *url, const char *headers_str, const char *body,
                      StreamCallback callback, void *userdata, int timeout_ms,
                      const volatile long *cancelled) {
    if (!url || !body || !callback) return false;

#ifdef _WIN32
    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};
    const char *slash_slash = strstr(url, "://");
    if (!slash_slash) return false;

    size_t scheme_len = slash_slash - url;
    if (scheme_len > 15) scheme_len = 15;
    strncpy(scheme, url, scheme_len);

    const char *host_start = slash_slash + 3;
    const char *path_start = strchr(host_start, '/');
    const char *colon = strchr(host_start, ':');

    if (path_start) {
        size_t host_len = path_start - host_start;
        if (colon && colon < path_start) {
            host_len = colon - host_start;
            port = atoi(colon + 1);
            if (port == 0) port = 443;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(path, path_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        size_t host_len = strlen(host_start);
        if (colon) {
            host_len = colon - host_start;
            port = atoi(colon + 1);
            if (port == 0) port = 443;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(path, "/", sizeof(path));
    }

    int use_ssl = (strcmp(scheme, "https") == 0) ? 1 : 0;

    wchar_t *w_host = utf8_to_wide(host);
    wchar_t *w_path = utf8_to_wide(path);

    HINTERNET hSession = WinHttpOpen(L"NanoClaude/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { free(w_host); free(w_path); return false; }

    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, w_host, (INTERNET_PORT)port, 0);
    free(w_host);
    if (!hConnect) { WinHttpCloseHandle(hSession); free(w_path); return false; }

    DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", w_path, NULL, NULL, NULL, flags);
    free(w_path);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (headers_str) {
        wchar_t *w_headers = utf8_to_wide(headers_str);
        if (w_headers) {
            WinHttpAddRequestHeaders(hRequest, w_headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            free(w_headers);
        }
    }

    if (use_ssl) {
        DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
    }

    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);
    if (!sent) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status_code, &size, NULL);

    if (status_code != 200) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    char chunk[4096];
    char *line_buf = NULL;
    size_t line_buf_len = 0;
    size_t line_buf_cap = 0;
    DWORD bytes_read;

    while (!cancelled || !*cancelled) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) break;
        if (available == 0) { Sleep(100); if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) break; }
        if (available > sizeof(chunk) - 1) available = sizeof(chunk) - 1;
        if (!WinHttpReadData(hRequest, chunk, available, &bytes_read)) break;
        if (bytes_read == 0) break;
        chunk[bytes_read] = '\0';

        size_t new_len = line_buf_len + bytes_read;
        if (new_len + 1 > line_buf_cap) {
            size_t new_cap = (new_len + 1) * 2;
            char *new_buf = realloc(line_buf, new_cap);
            if (!new_buf) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
            line_buf = new_buf;
            line_buf_cap = new_cap;
        }
        memcpy(line_buf + line_buf_len, chunk, bytes_read);
        line_buf_len += bytes_read;
        line_buf[line_buf_len] = '\0';

        char *ptr = line_buf;
        char *line_end;
        while ((line_end = strchr(ptr, '\n')) != NULL) {
            *line_end = '\0';
            size_t line_len = (size_t)(line_end - ptr);
            if (line_len > 0 && ptr[line_len - 1] == '\r') { line_len--; ptr[line_len] = '\0'; }
            if (strncmp(ptr, "data: ", 6) == 0) callback(ptr + 6, userdata);
            ptr = line_end + 1;
        }
        if (ptr > line_buf) {
            size_t remaining = line_buf_len - (ptr - line_buf);
            memmove(line_buf, ptr, remaining);
            line_buf_len = remaining;
            line_buf[line_buf_len] = '\0';
        }
    }

    if (line_buf && line_buf_len > 0 && strncmp(line_buf, "data: ", 6) == 0) callback(line_buf + 6, userdata);
    free(line_buf);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return true;

#else
    // Linux/macOS: libcurl
    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};
    parse_url(url, scheme, host, &port, path);

    char full_url[2048];
    snprintf(full_url, sizeof(full_url), "%s://%s:%d%s", scheme, host, port, path);

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *hdrs = NULL;
    if (headers_str) {
        char tmp[4096];
        strncpy(tmp, headers_str, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *save = NULL;
        char *line = strtok_r(tmp, "\n", &save);
        while (line) {
            if (*line) hdrs = curl_slist_append(hdrs, line);
            line = strtok_r(NULL, "\n", &save);
        }
    }

    SSECtx sse;
    sse_ctx_init(&sse, callback, userdata, cancelled);

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // Disable progress bar and signals for clean SSE streaming
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        sse_ctx_free(&sse);
        return false;
    }

    sse_ctx_free(&sse);
    return sse.ok;
#endif
}

