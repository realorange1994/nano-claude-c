#ifndef HTTP_H
#define HTTP_H

#include <windows.h>
#include <winhttp.h>

#include <stdbool.h>

typedef void (*StreamCallback)(const char *chunk, void *userdata);

// Initialize HTTP module
void http_init(void);
void http_cleanup(void);

// Synchronous HTTP POST
char *http_post(const char *url, const char *headers, const char *body, int timeout_ms);

// Streaming HTTP POST with SSE callback
// cancelled: if non-NULL, checked between reads to allow Ctrl+C interruption
bool http_post_stream(const char *url, const char *headers, const char *body,
                      StreamCallback callback, void *userdata, int timeout_ms,
                      const volatile long *cancelled);

// Synchronous GET
char *http_get(const char *url, int timeout_ms);

// Utility: parse SSE data line
char *parse_sse_data(const char *line);

#endif // HTTP_H
