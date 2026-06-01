#include "glob.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <windows.h>

// Max output size for glob (100KB)
#define MAX_GLOB_OUTPUT (100 * 1024)

// Max directory depth
#define MAX_DEPTH 20

// Check if output buffer is full
static int is_output_full(GlobResult *result) {
    return result->buf.len >= MAX_GLOB_OUTPUT;
}

// Match filename against glob pattern
static int glob_match(const char *filename, const char *pattern) {
    if (!filename || !pattern) return 0;

    const char *f = filename;
    const char *p = pattern;

    while (*p && *f) {
        if (*p == '*') {
            p++;  // Skip the '*', now p points to what comes after *
            // Try matching * against 0, 1, 2, ... characters
            // First try matching * against 0 chars (skip *)
            if (glob_match(f, p)) return 1;
            // Then try matching * against 1+ chars
            while (*f) {
                if (*f == '/' || *f == '\\') break;  // * doesn't cross path boundaries
                f++;
                if (glob_match(f, p)) return 1;
            }
            return 0;  // No match found for this *
        } else if (*p == '?') {
            if (*f == '/' || *f == '\\') return 0;
            p++;
            f++;
        } else {
            if (tolower((unsigned char)*p) != tolower((unsigned char)*f)) return 0;
            p++;
            f++;
        }
    }

    // Handle trailing *'s (they can match empty string)
    while (*p == '*') p++;

    return (*p == '\0' && *f == '\0');
}

// Simple stack entry for iterative directory walking
typedef struct DirEntry {
    char path[MAX_PATH];
    struct DirEntry *next;
} DirEntry;

// Skip known large/problematic directories
static int should_skip_dir(const char *dirname) {
    if (dirname[0] == '.' && dirname[1] == 'g' && strcmp(dirname, ".git") == 0) return 1;
    if (strcmp(dirname, "node_modules") == 0) return 1;
    if (strcmp(dirname, "__pycache__") == 0) return 1;
    if (dirname[0] == '.' && strcmp(dirname, ".cache") == 0) return 1;
    if (strcmp(dirname, ".claude") == 0) return 1;
    if (strcmp(dirname, "target") == 0) return 1;
    return 0;
}

// Walk directory iteratively (no recursion - avoids stack overflow)
static void walk_directory(const char *start_dir, const char *file_pattern, GlobConfig *cfg, GlobResult *result) {
    DirEntry *stack = NULL;

    // Push start directory
    DirEntry *first = calloc(1, sizeof(DirEntry));
    if (!first) return;
    strncpy(first->path, start_dir, MAX_PATH - 1);
    first->next = NULL;
    stack = first;

    while (stack && !is_output_full(result)) {
        // Pop directory from stack
        DirEntry *current = stack;
        stack = stack->next;
        char dir[MAX_PATH];
        strncpy(dir, current->path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = '\0';
        free(current);

        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\*", dir);

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);

        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (is_output_full(result)) break;

            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (cfg->exclude_hidden && fd.cFileName[0] == '.') continue;

            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);

            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            // Push subdirectory onto stack FIRST (before type filter, so we always recurse)
            if (is_dir && !should_skip_dir(fd.cFileName)) {
                DirEntry *sub = calloc(1, sizeof(DirEntry));
                if (sub) {
                    strncpy(sub->path, full_path, MAX_PATH - 1);
                    sub->next = stack;
                    stack = sub;
                }
            }

            // Apply type filter for output (but directories still get traversed above)
            if (cfg->type_filter == GLOB_TYPE_FILE && is_dir) continue;
            if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir) continue;

            // Check pattern match
            if (glob_match(fd.cFileName, file_pattern)) {
                buffer_append_str(&result->buf, full_path);
                buffer_append_str(&result->buf, "\n");
                result->count++;

                if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
            }
        } while (FindNextFileA(h, &fd) && !is_output_full(result));

        FindClose(h);
    }

    // Free any remaining stack entries
    while (stack) {
        DirEntry *next = stack->next;
        free(stack);
        stack = next;
    }
}

// Main glob function
GlobResult *glob_search(GlobConfig *cfg) {
    if (!cfg || !cfg->pattern) return NULL;

    GlobResult *result = calloc(1, sizeof(GlobResult));
    if (!result) return NULL;

    buffer_init(&result->buf);
    result->count = 0;

    const char *dir = cfg->path ? cfg->path : ".";
    char dir_buf[MAX_PATH];  // Buffer for constructed dir paths
    const char *pattern = cfg->pattern;
    int recursive = 0;

    // Handle ** patterns: ** means recursive, strip it from pattern
    // e.g. "**/*.c" -> recursive search for "*.c" starting from dir
    // e.g. "src/**/*.c" -> recursive search for "*.c" starting from "src"
    const char *doublestar = strstr(pattern, "**");
    if (doublestar) {
        recursive = 1;
        // Check what comes before **
        if (doublestar == pattern || (doublestar == pattern + 1 && pattern[0] == '/')) {
            // "**/*.c" or "/**/*.c" -> search from dir for *.c recursively
            const char *after = doublestar + 2;
            while (*after == '/') after++;  // skip slashes after **
            if (*after) {
                pattern = after;
            } else {
                pattern = "*";
            }
        } else {
            // "src/**/*.c" -> dir="src", pattern="*.c", recursive
            size_t prefix_len = doublestar - pattern;
            // Strip trailing slash from prefix
            while (prefix_len > 0 && (pattern[prefix_len-1] == '/' || pattern[prefix_len-1] == '\\'))
                prefix_len--;
            if (prefix_len > 0) {
                strncpy(dir_buf, pattern, prefix_len);
                dir_buf[prefix_len] = '\0';
                dir = dir_buf;
            }
            const char *after = doublestar + 2;
            while (*after == '/') after++;
            if (*after) {
                pattern = after;
            } else {
                pattern = "*";
            }
        }
    } else {
        // No ** - check if pattern has path separator for non-recursive patterns
        // e.g. "src/*.c" -> dir="src", pattern="*.c"
        const char *last_sep = strrchr(pattern, '/');
        if (!last_sep) last_sep = strrchr(pattern, '\\');

        if (last_sep && last_sep != pattern) {
            size_t dir_len = last_sep - pattern;
            strncpy(dir_buf, pattern, dir_len);
            dir_buf[dir_len] = '\0';

            if (dir_buf[0] == '/' || dir_buf[0] == '\\' ||
                (strlen(dir) == 1 && dir[0] == '.')) {
                dir = dir_buf;
            } else {
                char combined[MAX_PATH];
                snprintf(combined, sizeof(combined), "%s\\%s", dir, dir_buf);
                strncpy(dir_buf, combined, sizeof(dir_buf) - 1);
                dir = dir_buf;
            }
            pattern = last_sep + 1;
        }
    }

    if (recursive) {
        walk_directory(dir, pattern, cfg, result);
    } else {
        // Non-recursive
        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\%s", dir, pattern);
        if (!strchr(pattern, '*') && !strchr(pattern, '?')) {
            strncat(search_path, "*", sizeof(search_path) - strlen(search_path) - 1);
        }

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);

        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (is_output_full(result)) break;
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                if (cfg->exclude_hidden && fd.cFileName[0] == '.') continue;

                int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (cfg->type_filter == GLOB_TYPE_FILE && is_dir) continue;
                if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir) continue;

                if (glob_match(fd.cFileName, pattern)) {
                    char full_path[MAX_PATH];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
                    buffer_append_str(&result->buf, full_path);
                    buffer_append_str(&result->buf, "\n");
                    result->count++;

                    if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    return result;
}

char *glob_get_output(GlobResult *result) {
    if (!result || !result->buf.data) return strdup("No matches found.");
    if (result->count == 0) return strdup("No matches found.");
    char *data = result->buf.data;
    result->buf.data = NULL;
    result->buf.len = 0;
    result->buf.capacity = 0;
    return data;
}

void glob_free_result(GlobResult *result) {
    if (!result) return;
    buffer_free(&result->buf);
    free(result);
}
