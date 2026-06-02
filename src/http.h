#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

typedef void (*StreamCallback)(const char *chunk, void *userdata);

// Initialize HTTP module
void http_init(void);
void http_cleanup(void);

// Synchronous HTTP POST
char *http_post(const char *url, const char *headers, const char *body, int timeout_ms);

// Extended HTTP POST with status code and error details
typedef struct {
    char *body;          // response body (malloc'd), NULL on failure
    int  http_status;    // 0 on connection error, otherwise actual status code
    char error[512];     // error message if failed
} HttpResponse;

HttpResponse http_post_ex(const char *url, const char *headers, const char *body, int timeout_ms);

// Streaming HTTP POST with SSE callback
// cancelled: if non-NULL, checked between reads to allow Ctrl+C interruption
bool http_post_stream(const char *url, const char *headers, const char *body,
                      StreamCallback callback, void *userdata, int timeout_ms,
                      const volatile long *cancelled);

// Extended streaming POST with status code and error details
typedef struct {
    bool  success;
    int   http_status;    // 0 on connection error
    char  error[512];
} HttpStreamResult;

bool http_post_stream_ex(const char *url, const char *headers, const char *body,
                         StreamCallback callback, void *userdata, int timeout_ms,
                         const volatile long *cancelled, HttpStreamResult *result);


#endif // HTTP_H
