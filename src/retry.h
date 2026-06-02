#ifdef _MSC_VER
#pragma warning(disable:4819)
#endif
#ifndef RETRY_H
#define RETRY_H

#include <stdbool.h>

// Error classification for retry decisions
typedef enum {
    RETRY_RETRYABLE,       // 429, 5xx, connection error, timeout
    RETRY_NON_RETRYABLE,   // 400, 401, 404, billing, auth
    RETRY_CONTEXT_OVERFLOW // prompt too long -- needs compaction
} RetryDecision;

typedef struct {
    RetryDecision decision;
    int  retry_after_secs;  // from Retry-After header (0 if not present)
} RetryResult;

// Classify an HTTP error for retry decision
RetryResult classify_error(int http_status, const char *error_body);

// Exponential backoff with jitter: base * 2^attempt + uniform(0, base * 2^attempt / 2)
// Returns delay in milliseconds. Cap at 60s.
int retry_backoff_ms(int attempt);

// Sleep for given milliseconds (platform-specific)
void retry_sleep_ms(int ms);

// Retry state tracked per-provider
typedef struct {
    int  consecutive_errors;
    int  last_error_status;
    char last_error[512];
    int  stream_failures;   // consecutive streaming failures (for fallback)
} RetryState;

// Initialize/reset retry state
static inline void retry_state_init(RetryState *s) {
    s->consecutive_errors = 0;
    s->last_error_status = 0;
    s->last_error[0] = '\0';
    s->stream_failures = 0;
}

// Record a successful request (reset error counter)
static inline void retry_state_on_success(RetryState *s) {
    s->consecutive_errors = 0;
    s->last_error_status = 0;
    s->last_error[0] = '\0';
}

// Record a streaming success (reset stream failure counter)
static inline void retry_state_stream_success(RetryState *s) {
    s->stream_failures = 0;
}

// Record a streaming failure
static inline void retry_state_stream_failure(RetryState *s) {
    s->stream_failures++;
}

// Check if streaming fallback should be used
static inline bool retry_state_should_fallback(RetryState *s) {
    return s->stream_failures >= 3;
}

#endif // RETRY_H
