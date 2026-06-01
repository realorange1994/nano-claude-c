#include "glob.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

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
            p++;
            if (glob_match(f, p)) return 1;
            while (*f) {
                if (*f == '/' || *f == '\\') break;
                f++;
                if (glob_match(f, p)) return 1;
            }
            return 0;
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

    while (*p == '*') p++;

    return (*p == '\0' && *f == '\0');
}

// Max path buffer size
#ifdef _WIN32
#define MAX_P MAX_PATH
#else
#define MAX_P 4096
#endif

// Check if a filename is a directory
static int is_dir(const char *full_path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(full_path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    if (stat(full_path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
#endif
}

// Check if should skip directory
static int should_skip_dir(const char *dirname) {
    if (strcmp(dirname, ".git") == 0) return 1;
    if (strcmp(dirname, "node_modules") == 0) return 1;
    if (strcmp(dirname, "__pycache__") == 0) return 1;
    if (strcmp(dirname, ".cache") == 0) return 1;
    if (strcmp(dirname, ".claude") == 0) return 1;
    if (strcmp(dirname, "target") == 0) return 1;
    return 0;
}

// Walk directory iteratively
static void walk_directory(const char *start_dir, const char *file_pattern, GlobConfig *cfg, GlobResult *result) {
#ifdef _WIN32
    // Windows: FindFirstFile/FindNextFile
    typedef struct DirEntry {
        char path[MAX_P];
        struct DirEntry *next;
    } DirEntry;
    DirEntry *stack = NULL;

    DirEntry *first = calloc(1, sizeof(DirEntry));
    if (!first) return;
    strncpy(first->path, start_dir, MAX_P - 1);
    first->next = NULL;
    stack = first;

    while (stack && !is_output_full(result)) {
        DirEntry *current = stack;
        stack = stack->next;
        char dir[MAX_P];
        strncpy(dir, current->path, MAX_P - 1);
        dir[MAX_P - 1] = '\0';
        free(current);

        char search_path[MAX_P];
        snprintf(search_path, sizeof(search_path), "%s\\*", dir);

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (is_output_full(result)) break;
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (cfg->exclude_hidden && fd.cFileName[0] == '.') continue;

            char full_path[MAX_P];
            snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);

            int is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (is_directory && !should_skip_dir(fd.cFileName)) {
                DirEntry *sub = calloc(1, sizeof(DirEntry));
                if (sub) {
                    strncpy(sub->path, full_path, MAX_P - 1);
                    sub->next = stack;
                    stack = sub;
                }
            }

            if (cfg->type_filter == GLOB_TYPE_FILE && is_directory) continue;
            if (cfg->type_filter == GLOB_TYPE_DIR && !is_directory) continue;

            if (glob_match(fd.cFileName, file_pattern)) {
                buffer_append_str(&result->buf, full_path);
                buffer_append_str(&result->buf, "\n");
                result->count++;
                if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
            }
        } while (FindNextFileA(h, &fd) && !is_output_full(result));

        FindClose(h);
    }

    while (stack) {
        DirEntry *next = stack->next;
        free(stack);
        stack = next;
    }
#else
    // Linux: opendir/readdir
    typedef struct DirEntry {
        char path[MAX_P];
        struct DirEntry *next;
    } DirEntry;
    DirEntry *stack = NULL;

    DirEntry *first = calloc(1, sizeof(DirEntry));
    if (!first) return;
    strncpy(first->path, start_dir, MAX_P - 1);
    first->next = NULL;
    stack = first;

    while (stack && !is_output_full(result)) {
        DirEntry *current = stack;
        stack = stack->next;
        char dir[MAX_P];
        strncpy(dir, current->path, MAX_P - 1);
        dir[MAX_P - 1] = '\0';
        free(current);

        DIR *d = opendir(dir);
        if (!d) continue;

        struct dirent *entry;
        while ((entry = readdir(d)) != NULL && !is_output_full(result)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            if (cfg->exclude_hidden && entry->d_name[0] == '.') continue;

            char full_path[MAX_P];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

            int is_directory = is_dir(full_path);

            if (is_directory && !should_skip_dir(entry->d_name)) {
                DirEntry *sub = calloc(1, sizeof(DirEntry));
                if (sub) {
                    strncpy(sub->path, full_path, MAX_P - 1);
                    sub->next = stack;
                    stack = sub;
                }
            }

            if (cfg->type_filter == GLOB_TYPE_FILE && is_directory) continue;
            if (cfg->type_filter == GLOB_TYPE_DIR && !is_directory) continue;

            if (glob_match(entry->d_name, file_pattern)) {
                buffer_append_str(&result->buf, full_path);
                buffer_append_str(&result->buf, "\n");
                result->count++;
                if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
            }
        }
        closedir(d);
    }

    while (stack) {
        DirEntry *next = stack->next;
        free(stack);
        stack = next;
    }
#endif
}

// Main glob function
GlobResult *glob_search(GlobConfig *cfg) {
    if (!cfg || !cfg->pattern) return NULL;

    GlobResult *result = calloc(1, sizeof(GlobResult));
    if (!result) return NULL;

    buffer_init(&result->buf);
    result->count = 0;

    const char *dir = cfg->path ? cfg->path : ".";
    char dir_buf[MAX_P];
    const char *pattern = cfg->pattern;
    int recursive = 0;

    // Handle ** patterns
    const char *doublestar = strstr(pattern, "**");
    if (doublestar) {
        recursive = 1;
        if (doublestar == pattern || (doublestar == pattern + 1 && pattern[0] == '/')) {
            const char *after = doublestar + 2;
            while (*after == '/') after++;
            if (*after) {
                pattern = after;
            } else {
                pattern = "*";
            }
        } else {
            size_t prefix_len = doublestar - pattern;
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
                char combined[MAX_P];
                snprintf(combined, sizeof(combined), "%s/%s", dir, dir_buf);
                strncpy(dir_buf, combined, sizeof(dir_buf) - 1);
                dir = dir_buf;
            }
            pattern = last_sep + 1;
        }
    }

    if (recursive) {
        walk_directory(dir, pattern, cfg, result);
    } else {
        // Non-recursive: use the walk for a single directory listing
#ifdef _WIN32
        char search_path[MAX_P];
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

                int is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (cfg->type_filter == GLOB_TYPE_FILE && is_directory) continue;
                if (cfg->type_filter == GLOB_TYPE_DIR && !is_directory) continue;

                if (glob_match(fd.cFileName, pattern)) {
                    char full_path[MAX_P];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
                    buffer_append_str(&result->buf, full_path);
                    buffer_append_str(&result->buf, "\n");
                    result->count++;
                    if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
#else
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL && !is_output_full(result)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                if (cfg->exclude_hidden && entry->d_name[0] == '.') continue;

                char full_path[MAX_P];
                int is_directory = is_dir(full_path) && snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name) > 0;
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

                if (cfg->type_filter == GLOB_TYPE_FILE && is_dir(full_path)) continue;
                if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir(full_path)) continue;

                if (glob_match(entry->d_name, pattern)) {
                    buffer_append_str(&result->buf, full_path);
                    buffer_append_str(&result->buf, "\n");
                    result->count++;
                    if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
                }
            }
            closedir(d);
        }
#endif
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
