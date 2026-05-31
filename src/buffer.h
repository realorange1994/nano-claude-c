#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} Buffer;

// Buffer management
void buffer_init(Buffer *buf);
void buffer_free(Buffer *buf);
void buffer_clear(Buffer *buf);

// Growth strategy
void buffer_reserve(Buffer *buf, size_t capacity);
void buffer_append(Buffer *buf, const char *data, size_t len);
void buffer_append_str(Buffer *buf, const char *str);

// Line reading (for SSE parsing)
char *buffer_read_line(Buffer *buf);

// Utility
char *buffer_c_str(Buffer *buf);
bool buffer_starts_with(Buffer *buf, const char *prefix);
size_t buffer_len(Buffer *buf);

#endif // BUFFER_H
