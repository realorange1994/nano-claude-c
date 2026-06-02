#ifdef _MSC_VER
#pragma warning(disable:4819)
#endif
#include "retry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

// Case-insensitive substring check
static int contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    if (nlen == 0) return 0;
    for (const char *p = haystack; *p; p++) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            char hc = p[i];
            char nc = needle[i];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static int contains_any_ci(const char *text, const char **patterns, int count) {
    for (int i = 0; i < count; i++) {
        if (contains_ci(text, patterns[i])) return 1;
    }
    return 0;
}

// Context overflow patterns
static const char *overflow_patterns[] = {
    "prompt is too long", "context_length", "context window",
    "token limit", "max_tokens", "context_exceeded",
    "prompt_too_long", "input is too long", "reduce the length"
};

// Non-retryable patterns (auth, billing, model not found)
static const char *non_retryable_patterns[] = {
    "invalid api key", "invalid_api_key", "authentication",
    "unauthorized", "forbidden", "token expired", "access denied",
    "billing", "insufficient credits", "insufficient_quota",
    "credit balance", "credits have been exhausted", "payment required",
    "model not found", "invalid model", "model_not_found",
    "unknown model", "no such model"
};

// Retryable patterns (beyond status code matching)
static const char *retryable_patterns[] = {
    "overloaded", "rate limit", "rate_limit", "too many requests",
    "throttled", "connection error", "connection refused",
    "connection reset", "connection timed out", "no such host",
    "network error", "service unavailable", "bad gateway",
    "gateway timeout", "internal server error",
    "premature stream", "fetch failed", "request timeout"
};

RetryResult classify_error(int http_status, const char *error_body) {
    RetryResult result = {RETRY_RETRYABLE, 0};

    // Check error body for context overflow first (highest priority)
    if (error_body && error_body[0]) {
        int n_overflow = (int)(sizeof(overflow_patterns) / sizeof(overflow_patterns[0]));
        if (contains_any_ci(error_body, overflow_patterns, n_overflow)) {
            result.decision = RETRY_CONTEXT_OVERFLOW;
            return result;
        }
    }

    // Status code classification
    switch (http_status) {
        case 200:
        case 201:
            result.decision = RETRY_RETRYABLE; // shouldn't happen but treat as retryable
            return result;
        case 400:
            // 400 could be context overflow or format error
            if (error_body && error_body[0]) {
                int n_overflow = (int)(sizeof(overflow_patterns) / sizeof(overflow_patterns[0]));
                if (contains_any_ci(error_body, overflow_patterns, n_overflow)) {
                    result.decision = RETRY_CONTEXT_OVERFLOW;
                    return result;
                }
            }
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        case 401:
        case 403:
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        case 404:
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        case 402:
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        case 413:
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        case 429:
            result.decision = RETRY_RETRYABLE;
            // Retry-After would be parsed by caller from response headers
            return result;
        case 500:
        case 501:
        case 502:
            result.decision = RETRY_RETRYABLE;
            return result;
        case 503:
        case 529:
            result.decision = RETRY_RETRYABLE; // overloaded
            return result;
        case 504:
            result.decision = RETRY_RETRYABLE;
            return result;
        default:
            if (http_status >= 400 && http_status < 500) {
                result.decision = RETRY_NON_RETRYABLE;
                return result;
            }
            if (http_status >= 500) {
                result.decision = RETRY_RETRYABLE;
                return result;
            }
            break;
    }

    // No status code (network error) -- check error body
    if (error_body && error_body[0]) {
        int n_non = (int)(sizeof(non_retryable_patterns) / sizeof(non_retryable_patterns[0]));
        if (contains_any_ci(error_body, non_retryable_patterns, n_non)) {
            result.decision = RETRY_NON_RETRYABLE;
            return result;
        }
        int n_retry = (int)(sizeof(retryable_patterns) / sizeof(retryable_patterns[0]));
        if (contains_any_ci(error_body, retryable_patterns, n_retry)) {
            result.decision = RETRY_RETRYABLE;
            return result;
        }
    }

    // Connection error (http_status == 0) is retryable
    if (http_status == 0) {
        result.decision = RETRY_RETRYABLE;
        return result;
    }

    // Unknown error -- don't retry
    result.decision = RETRY_NON_RETRYABLE;
    return result;
}

int retry_backoff_ms(int attempt) {
    // base * 2^attempt + uniform(0, base * 2^attempt / 2)
    // base = 2000ms, cap at 60000ms
    int base = 2000;
    int exp = 1 << attempt;  // 2^attempt
    int delay = base * exp;
    if (delay > 60000) delay = 60000;

    // Add jitter: uniform(0, delay / 2)
    int jitter = 0;
#ifdef _WIN32
    jitter = rand() % (delay / 2 + 1);
#else
    jitter = rand() % (delay / 2 + 1);
#endif

    return delay + jitter;
}

void retry_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}
