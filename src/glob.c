#include "glob.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <windows.h>

// Match filename against glob pattern
static int glob_match(const char *filename, const char *pattern) {
    const char *f = filename;
    const char *p = pattern;
    
    while (*p && *f) {
        if (*p == '*') {
            // Match zero or more characters (but not across path separators)
            p++;
            if (*p == '/') {
                // * doesn't cross directory boundaries
                while (*f && *f != '/') f++;
                if (*f == '/') f++;
            } else if (*p == '\\') {
                // Same for backslash on Windows
                while (*f && *f != '\\' && *f != '/') f++;
                if (*f == '\\' || *f == '/') f++;
            } else if (*p == '*') {
                // Double star - matches everything including path separators
                // Find next character in pattern
                const char *next = p + 1;
                while (*next && *next != '*' && *next != '?' && *next != '/' && *next != '\\') {
                    next++;
                }
                // Match until we find the next pattern char
                while (*f) {
                    if (next && *next) {
                        // Look ahead for the next pattern char
                        const char *test = f;
                        const char *np = next;
                        int match = 1;
                        while (*np && *test) {
                            if (*np == '*' || *np == '?') break;
                            if (*np != *test) {
                                match = 0;
                                break;
                            }
                            np++;
                            test++;
                        }
                        if (match && (*np == '\0' || *np == '*' || *np == '?' || *test == '\0')) {
                            // Found a match
                            f = test;
                            p = next;
                            break;
                        }
                    }
                    f++;
                }
            } else {
                // Single star - match any characters except path separator
                while (*f && *f != '/' && *f != '\\') {
                    // Check if remaining pattern matches here
                    const char *test_f = f;
                    const char *test_p = p;
                    int matched = 1;
                    while (*test_p && *test_f) {
                        if (*test_p == '*') break;
                        if (*test_p == '?') {
                            test_p++;
                            test_f++;
                            continue;
                        }
                        if (*test_p != *test_f) {
                            matched = 0;
                            break;
                        }
                        test_p++;
                        test_f++;
                    }
                    if (matched && (*test_p == '\0' || *test_p == '*' || *test_p == '?')) {
                        // Match found
                        p = test_p;
                        f = test_f;
                        break;
                    }
                    f++;
                }
            }
        } else if (*p == '?') {
            // Match any single character except path separator
            if (*f == '/' || *f == '\\') {
                return 0;
            }
            p++;
            f++;
        } else {
            // Literal character match (case-insensitive)
            if (tolower((unsigned char)*p) != tolower((unsigned char)*f)) {
                return 0;
            }
            p++;
            f++;
        }
    }
    
    // Handle trailing wildcards
    while (*p == '*') p++;
    
    // If pattern exhausted, filename should too
    return (*p == '\0' && *f == '\0');
}

// Match path against glob pattern (handles ** for recursive)
static int path_match(const char *path, const char *pattern) {
    // Handle ** patterns
    const char *double_star = strstr(pattern, "**");
    if (double_star) {
        // Split pattern at **
        size_t prefix_len = double_star - pattern;
        char prefix[512] = {0};
        strncpy(prefix, pattern, prefix_len);
        prefix[prefix_len] = '\0';
        
        const char *rest = double_star + 2;
        while (*rest == '/' || *rest == '\\') rest++;
        
        // Find prefix in path
        const char *found = path;
        if (prefix[0]) {
            found = strstr(path, prefix);
            if (!found) return 0;
        }
        
        // Check if rest of pattern matches somewhere in the remaining path
        while (found) {
            const char *remaining = found + (prefix[0] ? strlen(prefix) : 0);
            // Skip path separators
            while (*remaining == '/' || *remaining == '\\') remaining++;
            
            if (rest[0] == '\0') {
                // Pattern ends with **
                return 1;
            }
            
            // Try to match rest of pattern
            const char *p = rest;
            const char *f = remaining;
            while (*p && *f) {
                if (*p == '*') {
                    p++;
                    while (*f && *f != '/' && *f != '\\') f++;
                } else if (*p == '?') {
                    if (*f == '/' || *f == '\\') return 0;
                    p++;
                    f++;
                } else if (*p == '/' || *p == '\\') {
                    while (*f == '/' || *f == '\\') f++;
                    while (*p == '/' || *p == '\\') p++;
                } else {
                    if (tolower((unsigned char)*p) != tolower((unsigned char)*f)) {
                        break;
                    }
                    p++;
                    f++;
                }
            }
            while (*p == '*') p++;
            while (*p == '/' || *p == '\\') p++;
            
            if (*p == '\0' && *f == '\0') return 1;
            if (*p == '\0' && (*f == '/' || *f == '\\')) return 1;
            
            // Try next occurrence
            found = strstr(found + 1, prefix);
        }
        
        return 0;
    }
    
    // Simple pattern - match against just filename
    const char *filename = strrchr(path, '/');
    if (!filename) filename = strrchr(path, '\\');
    if (!filename) filename = path;
    else filename++;
    
    return glob_match(filename, pattern);
}

// Walk directory recursively
static void walk_directory(const char *dir, GlobConfig *cfg, GlobResult *result) {
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", dir);
    
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search_path, &fd);
    
    if (h == INVALID_HANDLE_VALUE) return;
    
    do {
        // Skip . and ..
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        
        // Skip hidden files
        if (cfg->exclude_hidden && fd.cFileName[0] == '.') {
            continue;
        }
        
        // Build full path
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
        
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        
        // Type filter
        if (cfg->type_filter == GLOB_TYPE_FILE && is_dir) {
            continue;
        }
        if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir) {
            continue;
        }
        
        // Match pattern
        if (path_match(fd.cFileName, cfg->pattern)) {
            buffer_append_str(&result->buf, full_path);
            buffer_append_str(&result->buf, "\n");
            result->count++;
            
            // Check limit
            if (cfg->max_results > 0 && result->count >= cfg->max_results) {
                FindClose(h);
                return;
            }
        }
        
        // Recurse into directories
        if (is_dir) {
            walk_directory(full_path, cfg, result);
            
            // Check limit again after recursion
            if (cfg->max_results > 0 && result->count >= cfg->max_results) {
                break;
            }
        }
    } while (FindNextFileA(h, &fd));
    
    FindClose(h);
}

// Main glob function
GlobResult *glob_search(GlobConfig *cfg) {
    GlobResult *result = calloc(1, sizeof(GlobResult));
    if (!result) return NULL;
    
    buffer_init(&result->buf);
    result->count = 0;
    
    // Parse pattern to extract directory and file pattern
    const char *pattern = cfg->pattern;
    const char *dir = cfg->path ? cfg->path : ".";
    
    // Find the directory part of the pattern
    const char *last_slash = NULL;
    const char *p = pattern;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_slash = p;
        }
        p++;
    }
    
    if (last_slash) {
        // Extract directory from pattern
        size_t dir_len = last_slash - pattern;
        char dir_from_pattern[512] = {0};
        strncpy(dir_from_pattern, pattern, dir_len);
        
        // Combine with path if needed
        if (dir_from_pattern[0] == '/' || dir_from_pattern[0] == '\\' || 
            (strlen(dir) == 1 && dir[0] == '.')) {
            // Absolute or root-relative pattern
            dir = dir_from_pattern;
        } else {
            // Relative pattern - combine
            if (strcmp(dir, ".") == 0) {
                dir = dir_from_pattern;
            } else {
                // Concatenate
                char combined[MAX_PATH];
                snprintf(combined, sizeof(combined), "%s\\%s", dir, dir_from_pattern);
                strncpy(dir_from_pattern, combined, sizeof(dir_from_pattern) - 1);
                dir = dir_from_pattern;
            }
        }
        pattern = last_slash + 1;
    }
    
    // Handle ** for recursive search
    if (strstr(cfg->pattern, "**")) {
        walk_directory(dir, cfg, result);
    } else {
        // Non-recursive search
        char search_path[MAX_PATH];
        if (strchr(pattern, '*') || strchr(pattern, '?')) {
            snprintf(search_path, sizeof(search_path), "%s\\%s", dir, pattern);
        } else {
            snprintf(search_path, sizeof(search_path), "%s\\%s*", dir, pattern);
        }
        
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);
        
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
                    continue;
                }
                
                if (cfg->exclude_hidden && fd.cFileName[0] == '.') {
                    continue;
                }
                
                // Type filter
                int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (cfg->type_filter == GLOB_TYPE_FILE && is_dir) {
                    continue;
                }
                if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir) {
                    continue;
                }
                
                char full_path[MAX_PATH];
                snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
                
                buffer_append_str(&result->buf, full_path);
                buffer_append_str(&result->buf, "\n");
                result->count++;
                
            } while (FindNextFileA(h, &fd) && 
                     (cfg->max_results == 0 || result->count < cfg->max_results));
            
            FindClose(h);
        }
    }
    
    return result;
}

void glob_free_result(GlobResult *result) {
    if (!result) return;
    buffer_free(&result->buf);
    free(result);
}

char *glob_get_output(GlobResult *result) {
    if (!result) return strdup("");
    if (result->count == 0) return strdup("No matches found.");
    return buffer_c_str(&result->buf);
}
