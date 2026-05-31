#include "http.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#pragma comment(lib, "winhttp.lib")

void http_init(void) {
}

void http_cleanup(void) {
}

static wchar_t *utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t *result = malloc(size * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result, size);
    return result;
}

char *http_post(const char *url, const char *headers, const char *body, int timeout_ms) {
    if (!url || !body) return NULL;

    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};

    if (sscanf(url, "%15[^://]://%255[^:/]:%d/%1023[^\n]", scheme, host, &port, path) != 4) {
        if (sscanf(url, "%15[^://]://%255[^/\n]", scheme, host) >= 2) {
            port = 443;
            strcpy(path, "/");
        } else {
            return NULL;
        }
    }

    int use_ssl = (strcmp(scheme, "https") == 0) ? 1 : 0;
    if (port == 0) port = use_ssl ? 443 : 80;

    wchar_t *w_host = utf8_to_wide(host);
    wchar_t *w_path = utf8_to_wide(path);

    HINTERNET hSession = WinHttpOpen(L"NanoClaude/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        free(w_host); free(w_path);
        return NULL;
    }

    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, w_host, (INTERNET_PORT)port, 0);
    free(w_host);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        free(w_path);
        return NULL;
    }

    DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", w_path, NULL, NULL, NULL, flags);
    free(w_path);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    if (headers) {
        wchar_t *w_headers = utf8_to_wide(headers);
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
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    WinHttpReceiveResponse(hRequest, NULL);

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status_code, &size, NULL);

    if (status_code != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    Buffer buf;
    buffer_init(&buf);

    char chunk[4096];
    DWORD bytes_read;

    while (WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &bytes_read) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        buffer_append(&buf, chunk, bytes_read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    char *response = buffer_c_str(&buf);
    buffer_free(&buf);

    return response;
}

bool http_post_stream(const char *url, const char *headers, const char *body,
                    StreamCallback callback, void *userdata, int timeout_ms,
                    const volatile long *cancelled) {
    if (!url || !body || !callback) return false;

    const char *slash_slash = strstr(url, "://");
    if (!slash_slash) return false;
    
    char scheme[16] = {0};
    size_t scheme_len = slash_slash - url;
    if (scheme_len > 15) scheme_len = 15;
    strncpy(scheme, url, scheme_len);
    
    const char *host_start = slash_slash + 3;
    const char *path_start = strchr(host_start, '/');
    const char *colon = strchr(host_start, ':');
    
    char host[256] = {0};
    char path[1024] = {0};
    int port = 443;
    
    if (path_start) {
        size_t host_len = path_start - host_start;
        if (colon && colon < path_start) {
            host_len = colon - host_start;
            port = atoi(colon + 1);
            if (port == 0) port = 443;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        strcpy(path, path_start);
    } else {
        size_t host_len = strlen(host_start);
        if (colon) {
            host_len = colon - host_start;
            port = atoi(colon + 1);
            if (port == 0) port = 443;
        }
        if (host_len > 255) host_len = 255;
        strncpy(host, host_start, host_len);
        strcpy(path, "/");
    }
    
    int use_ssl = (strcmp(scheme, "https") == 0) ? 1 : 0;

    wchar_t *w_host = utf8_to_wide(host);
    wchar_t *w_path = utf8_to_wide(path);

    HINTERNET hSession = WinHttpOpen(L"NanoClaude/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        free(w_host); free(w_path);
        return false;
    }

    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, w_host, (INTERNET_PORT)port, 0);
    free(w_host);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        free(w_path);
        return false;
    }

    DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", w_path, NULL, NULL, NULL, flags);
    free(w_path);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (headers) {
        wchar_t *w_headers = utf8_to_wide(headers);
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

    if (!sent) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status_code, &size, NULL);

    if (status_code != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // SSE parsing: dispatch each data line directly (stripped of "data: " prefix)
    // Use shorter timeouts and check cancelled flag for Ctrl+C support
    // Use a continuation buffer to handle SSE lines that span multiple chunks.
    // This is critical for tool call input_json_delta which can be very large.
    char chunk[4096];
    char *line_buf = NULL;
    size_t line_buf_len = 0;
    size_t line_buf_cap = 0;
    DWORD bytes_read;

    while (!cancelled || !*cancelled) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) {
            break;
        }
        if (available == 0) {
            Sleep(100);
            if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) {
                break;
            }
        }

        if (available > sizeof(chunk) - 1) {
            available = sizeof(chunk) - 1;
        }

        if (!WinHttpReadData(hRequest, chunk, available, &bytes_read)) {
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        chunk[bytes_read] = '\0';

        // Append new data to line buffer
        size_t new_len = line_buf_len + bytes_read;
        if (new_len > line_buf_cap) {
            line_buf_cap = (new_len + 1) * 2;
            char *new_buf = realloc(line_buf, line_buf_cap);
            if (new_buf) {
                line_buf = new_buf;
            } else {
                break;  // Out of memory
            }
        }
        if (line_buf) {
            memcpy(line_buf + line_buf_len, chunk, bytes_read + 1);
        }
        line_buf_len = new_len;

        // Process complete lines from buffer
        char *ptr = line_buf;
        char *line_end;

        while ((line_end = strchr(ptr, '\n')) != NULL) {
            *line_end = '\0';
            size_t line_len = (size_t)(line_end - ptr);
            if (line_len > 0 && ptr[line_len - 1] == '\r') {
                line_len--;
                ptr[line_len] = '\0';
            }

            // Only dispatch data: lines
            if (strncmp(ptr, "data: ", 6) == 0) {
                callback(ptr + 6, userdata);
            }
            // Skip event:, :, empty lines, etc.

            ptr = line_end + 1;
        }

        // Shift remaining incomplete line to start of buffer
        if (ptr > line_buf && ptr <= line_buf + line_buf_len) {
            size_t remaining = line_buf_len - (ptr - line_buf);
            memmove(line_buf, ptr, remaining + 1);
            line_buf_len = remaining;
        }
    }

    // Flush any remaining data in line buffer
    if (line_buf && line_buf_len > 0 && strncmp(line_buf, "data: ", 6) == 0) {
        callback(line_buf + 6, userdata);
    }
    free(line_buf);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}

char *http_get(const char *url, int timeout_ms) {
    if (!url) return NULL;

    char scheme[16] = {0};
    char host[256] = {0};
    int port = 443;
    char path[1024] = {0};

    if (sscanf(url, "%15[^://]://%255[^:/]:%d/%1023[^\n]", scheme, host, &port, path) != 4) {
        if (sscanf(url, "%15[^://]://%255[^/\n]", scheme, host) >= 2) {
            port = 443;
            strcpy(path, "/");
        } else {
            return NULL;
        }
    }

    int use_ssl = (strcmp(scheme, "https") == 0) ? 1 : 0;
    if (port == 0) port = use_ssl ? 443 : 80;

    wchar_t *w_host = utf8_to_wide(host);
    wchar_t *w_path = utf8_to_wide(path);

    HINTERNET hSession = WinHttpOpen(L"NanoClaude/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        free(w_host); free(w_path);
        return NULL;
    }

    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, w_host, (INTERNET_PORT)port, 0);
    free(w_host);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        free(w_path);
        return NULL;
    }

    DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", w_path, NULL, NULL, NULL, flags);
    free(w_path);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    if (use_ssl) {
        DWORD opts = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &opts, sizeof(opts));
    }

    BOOL result = WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);

    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return NULL;
    }

    WinHttpReceiveResponse(hRequest, NULL);

    Buffer buf;
    buffer_init(&buf);

    char chunk[4096];
    DWORD bytes_read;

    while (WinHttpReadData(hRequest, chunk, sizeof(chunk) - 1, &bytes_read) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        buffer_append(&buf, chunk, bytes_read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    char *response = buffer_c_str(&buf);
    buffer_free(&buf);

    return response;
}

char *parse_sse_data(const char *line) {
    if (!line) return NULL;
    if (strncmp(line, "data: ", 6) == 0) {
        return strdup(line + 6);
    }
    return NULL;
}
