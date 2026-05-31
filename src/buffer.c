#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const size_t INITIAL_CAPACITY = 256;
static const size_t GROWTH_FACTOR = 2;

void buffer_init(Buffer *buf) {
    buf->data = malloc(INITIAL_CAPACITY);
    if (!buf->data) {
        buf->data = malloc(INITIAL_CAPACITY);  // retry once
        if (!buf->data) {
            buf->data = NULL;
            buf->len = 0;
            buf->capacity = 0;
            return;
        }
    }
    buf->data[0] = '\0';
    buf->len = 0;
    buf->capacity = INITIAL_CAPACITY;
}

void buffer_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

void buffer_clear(Buffer *buf) {
    buf->len = 0;
    if (buf->data) {
        buf->data[0] = '\0';
    }
}

void buffer_reserve(Buffer *buf, size_t capacity) {
    if (capacity > buf->capacity) {
        size_t new_capacity = buf->capacity;
        while (new_capacity < capacity) {
            new_capacity *= GROWTH_FACTOR;
        }
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) {
            // Try smaller allocation
            new_capacity = capacity;
            new_data = realloc(buf->data, new_capacity);
            if (!new_data) return;  // Fail gracefully
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
}

void buffer_append(Buffer *buf, const char *data, size_t len) {
    if (len == 0 || !buf || !buf->data || !data) return;
    
    size_t needed = buf->len + len + 1;
    if (needed > buf->capacity) {
        buffer_reserve(buf, needed);
        if (needed > buf->capacity) return;  // Failed to allocate
    }
    
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

void buffer_append_str(Buffer *buf, const char *str) {
    if (!str) return;
    buffer_append(buf, str, strlen(str));
}

char *buffer_read_line(Buffer *buf) {
    // Find newline character
    char *newline = memchr(buf->data, '\n', buf->len);
    if (!newline) {
        return NULL;  // No complete line
    }
    
    size_t line_len = newline - buf->data;
    
    // Remove trailing \r if present
    if (line_len > 0 && buf->data[line_len - 1] == '\r') {
        line_len--;
    }
    
    // Allocate copy
    char *line = malloc(line_len + 1);
    if (!line) {
        fprintf(stderr, "buffer_read_line: malloc failed\n");
        return NULL;
    }
    
    memcpy(line, buf->data, line_len);
    line[line_len] = '\0';
    
    // Remove line from buffer
    size_t remaining = buf->len - (line_len + 1);
    if (remaining > 0) {
        memmove(buf->data, newline + 1, remaining);
    }
    buf->len = remaining;
    buf->data[buf->len] = '\0';
    
    return line;
}

char *buffer_c_str(Buffer *buf) {
    return buf->data ? buf->data : "";
}

size_t buffer_len(Buffer *buf) {
    return buf ? buf->len : 0;
}

bool buffer_starts_with(Buffer *buf, const char *prefix) {
    if (!buf->data || !prefix) return false;
    size_t prefix_len = strlen(prefix);
    return buf->len >= prefix_len && memcmp(buf->data, prefix, prefix_len) == 0;
}
