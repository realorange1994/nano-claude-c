#include "glob.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <windows.h>

// Max output size for glob (100KB)
#define MAX_GLOB_OUTPUT (100 * 1024)

// Check if output buffer is full
static int is_output_full(GlobResult *result) {
    return result->buf.len >= MAX_GLOB_OUTPUT;
}

// Match filename against glob pattern (simple implementation)
static int glob_match(const char *filename, const char *pattern) {
    if (!filename || !pattern || !*filename || !*pattern) return 0;
    
    // Simple recursive matcher with backtracking
    const char *f = filename;
    const char *p = pattern;
    
    while (*p && *f) {
        if (*p == '*') {
            // Try matching with zero chars first, then backtrack
            p++;
            // Try each position in filename
            const char *try_f = f;
            while (*try_f) {
                if (glob_match(try_f, p)) return 1;
                if (*try_f == '/' || *try_f == '\\') break;  // * doesn't cross dirs
                try_f++;
            }
            return glob_match(try_f, p);  // Try with zero chars
        } else if (*p == '?') {
            // Match any single char except path separator
            if (*f == '/' || *f == '\\') return 0;
            p++;
            f++;
        } else {
            // Literal match (case-insensitive)
            if (tolower((unsigned char)*p) != tolower((unsigned char)*f)) return 0;
            p++;
            f++;
        }
    }
    
    // Skip remaining * in pattern
    while (*p == '*') p++;
    
    return (*p == '\0' && *f == '\0');
}

// Walk directory
static void walk_directory(const char *dir, GlobConfig *cfg, GlobResult *result) {
    if (is_output_full(result)) return;
    
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", dir);
    
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search_path, &fd);
    
    if (h == INVALID_HANDLE_VALUE) return;
    
    do {
        if (is_output_full(result)) break;
        
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (cfg->exclude_hidden && fd.cFileName[0] == '.') continue;
        
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
        
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        
        if (cfg->type_filter == GLOB_TYPE_FILE && is_dir) continue;
        if (cfg->type_filter == GLOB_TYPE_DIR && !is_dir) continue;
        
        // Check pattern match
        if (glob_match(fd.cFileName, cfg->pattern)) {
            buffer_append_str(&result->buf, full_path);
            buffer_append_str(&result->buf, "\n");
            result->count++;
            
            if (cfg->max_results > 0 && result->count >= cfg->max_results) break;
        }
        
        // Recurse into directories (for ** patterns)
        if (is_dir) {
            walk_directory(full_path, cfg, result);
        }
    } while (FindNextFileA(h, &fd) && !is_output_full(result));
    
    FindClose(h);
}

// Main glob function
GlobResult *glob_search(GlobConfig *cfg) {
    if (!cfg || !cfg->pattern) return NULL;
    
    GlobResult *result = calloc(1, sizeof(GlobResult));
    if (!result) return NULL;
    
    buffer_init(&result->buf);
    result->count = 0;
    
    const char *dir = cfg->path ? cfg->path : ".";
    const char *pattern = cfg->pattern;
    
    // Check if pattern has path separator
    const char *last_sep = strrchr(pattern, '/');
    if (!last_sep) last_sep = strrchr(pattern, '\\');
    
    if (last_sep && last_sep != pattern) {
        // Pattern includes directory, extract it
        size_t dir_len = last_sep - pattern;
        char dir_part[512] = {0};
        strncpy(dir_part, pattern, dir_len);
        
        // Combine dir with cfg->path
        if (dir_part[0] == '/' || dir_part[0] == '\\' || 
            (strlen(dir) == 1 && dir[0] == '.')) {
            dir = dir_part;
        } else {
            char combined[MAX_PATH];
            snprintf(combined, sizeof(combined), "%s\\%s", dir, dir_part);
            strncpy(dir_part, combined, sizeof(dir_part) - 1);
            dir = dir_part;
        }
        pattern = last_sep + 1;
    }
    
    // Check if recursive (**)
    if (strstr(cfg->pattern, "**")) {
        walk_directory(dir, cfg, result);
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
    if (!result) return strdup("");
    if (result->count == 0) return strdup("No matches found.");
    return buffer_c_str(&result->buf);
}

void glob_free_result(GlobResult *result) {
    if (!result) return;
    buffer_free(&result->buf);
    free(result);
}
