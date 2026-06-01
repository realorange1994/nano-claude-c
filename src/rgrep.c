#include "rgrep.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <windows.h>

// File type to extension mappings (similar to ripgrep)
static const char *g_file_types[][2] = {
    {"go", ".go"},
    {"c", ".c"},
    {"cpp", ".cpp"},
    {"h", ".h"},
    {"py", ".py"},
    {"js", ".js"},
    {"ts", ".ts"},
    {"rs", ".rs"},
    {"java", ".java"},
    {"rb", ".rb"},
    {"php", ".php"},
    {"sh", ".sh"},
    {"bash", ".bash"},
    {"zsh", ".zsh"},
    {"html", ".html"},
    {"htm", ".htm"},
    {"css", ".css"},
    {"scss", ".scss"},
    {"sass", ".sass"},
    {"less", ".less"},
    {"json", ".json"},
    {"xml", ".xml"},
    {"yaml", ".yaml"},
    {"yml", ".yml"},
    {"toml", ".toml"},
    {"ini", ".ini"},
    {"cfg", ".cfg"},
    {"conf", ".conf"},
    {"md", ".md"},
    {"markdown", ".markdown"},
    {"txt", ".txt"},
    {"log", ".log"},
    {"sql", ".sql"},
    {"lua", ".lua"},
    {"pl", ".pl"},
    {"pm", ".pm"},
    {"vim", ".vim"},
    {"vimrc", ".vimrc"},
    {"bat", ".bat"},
    {"ps1", ".ps1"},
    {"psm1", ".psm1"},
    {"psd1", ".psd1"},
    {"asm", ".asm"},
    {"s", ".s"},
    {"S", ".S"},
    {"swift", ".swift"},
    {"kt", ".kt"},
    {"kts", ".kts"},
    {"scala", ".scala"},
    {"groovy", ".groovy"},
    {"gradle", ".gradle"},
    {"make", "Makefile"},
    {"makefile", "makefile"},
    {"cmake", "CMakeLists.txt"},
    {"dockerfile", "Dockerfile"},
    {"xml", ".xml"},
    {"xsd", ".xsd"},
    {"xsl", ".xsl"},
    {"xslt", ".xslt"},
    {"proto", ".proto"},
    {"tf", ".tf"},
    {"tfvars", ".tfvars"},
    {NULL, NULL}
};

// Check if file matches file type
static int type_matches(const char *path, const char *type) {
    const char *filename = strrchr(path, '\\');
    if (!filename) filename = strrchr(path, '/');
    if (!filename) filename = path;
    else filename++;
    
    size_t path_len = strlen(path);
    
    for (int i = 0; g_file_types[i][0]; i++) {
        if (strcmp(g_file_types[i][0], type) == 0) {
            const char *ext = g_file_types[i][1];
            // Check if it's a full filename (like Makefile, Dockerfile)
            if (ext[0] && isupper((unsigned char)ext[0])) {
                if (strcmp(filename, ext) == 0) {
                    return 1;
                }
            } else {
                // Check extension
                size_t ext_len = strlen(ext);
                if (path_len >= ext_len) {
                    const char *suffix = path + path_len - ext_len;
                    // Case-insensitive comparison
                    int match = 1;
                    for (size_t j = 0; j < ext_len; j++) {
                        if (tolower((unsigned char)suffix[j]) != tolower((unsigned char)ext[j])) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) return 1;
                }
            }
        }
    }
    return 0;
}

// Check if file matches glob pattern
static int glob_matches(const char *filename, const char *glob) {
    // Simple glob matching: * matches any characters, ? matches single char
    const char *g = glob;
    const char *f = filename;
    
    while (*g && *f) {
        if (*g == '*') {
            // Try matching 0 or more characters
            if (glob_matches(f, g + 1)) return 1;
            f++;
        } else if (*g == '?') {
            // Match any single character
            g++;
            f++;
        } else {
            // Literal match (case-insensitive)
            if (tolower((unsigned char)*g) != tolower((unsigned char)*f)) {
                // Check for **
                if (g[0] == '*' && g[1] == '*') {
                    // Skip **
                    g += 2;
                    if (*g == '/' || *g == '\\') g++;
                    continue;
                }
                return 0;
            }
            g++;
            f++;
        }
    }
    
    // Handle trailing *
    while (*g == '*') g++;
    
    return (*g == *f);
}

// Max output size for grep (500KB)
#define MAX_GREP_OUTPUT (500 * 1024)

// Check if output buffer is full
static int is_output_full(RGrepResult *result) {
    return result->buf.len >= MAX_GREP_OUTPUT;
}

// Check if file should be searched based on filters
static int should_search_file(const char *path, RGrepConfig *cfg) {
    const char *filename = strrchr(path, '\\');
    if (!filename) filename = strrchr(path, '/');
    if (!filename) filename = path;
    else filename++;
    
    // Check glob filter
    if (cfg->glob && cfg->glob[0]) {
        if (!glob_matches(filename, cfg->glob)) {
            return 0;
        }
    }
    
    // Check file type filter
    if (cfg->file_type && cfg->file_type[0]) {
        if (!type_matches(path, cfg->file_type)) {
            return 0;
        }
    }
    
    // Check if binary file
    if (!cfg->include_binary) {
        FILE *f = fopen(path, "rb");
        if (f) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            
            // Check for null bytes (binary indicator)
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\0') {
                    return 0;
                }
            }
        }
    }
    
    return 1;
}


// Simple substring match (no regex support for safety)
static int regex_matches(const char *text, const char *pattern, int case_sensitive) {
    if (!text || !pattern || !text[0] || !pattern[0]) return 0;
    
    if (case_sensitive) {
        return strstr(text, pattern) != NULL;
    } else {
        // Case-insensitive search
        size_t pattern_len = strlen(pattern);
        size_t text_len = strlen(text);
        if (pattern_len > text_len) return 0;
        
        for (size_t i = 0; i <= text_len - pattern_len; i++) {
            int match = 1;
            for (size_t j = 0; j < pattern_len; j++) {
                if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)pattern[j])) {
                    match = 0;
                    break;
                }
            }
            if (match) return 1;
        }
        return 0;
    }
}

// Search a single file
static void search_file(const char *filepath, RGrepConfig *cfg, RGrepResult *result) {
    // Check if output is already full
    if (is_output_full(result)) return;

    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    char line[8192];
    int line_num = 0;
    int file_matches = 0;

    while (fgets(line, sizeof(line), f)) {
        // Check if output is full before processing
        if (is_output_full(result)) break;

        line_num++;

        // Strip null bytes (binary data may have slipped through)
        for (char *p = line; *p; p++) {
            if (*p == '\0') *p = ' ';
        }

        // Truncate line if too long
        if (cfg->max_line_length > 0 && (int)strlen(line) > cfg->max_line_length) {
            line[cfg->max_line_length] = '\0';
        }

        // Remove trailing newline/carriage return
        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }

        if (line_len == 0) continue;  // Skip empty lines

        // Check for match
        if (regex_matches(line, cfg->pattern, cfg->case_sensitive)) {
            file_matches++;

            // Check max count per file
            if (cfg->max_count > 0 && file_matches > cfg->max_count) {
                break;
            }

            // Check output buffer size before appending
            if (is_output_full(result)) {
                fclose(f);
                return;
            }

            // Format based on output mode
            switch (cfg->output_mode) {
                case OUTPUT_FILES:
                    buffer_append_str(&result->buf, filepath);
                    buffer_append_str(&result->buf, "\n");
                    break;

                case OUTPUT_COUNT:
                    buffer_append_str(&result->buf, filepath);
                    buffer_append_str(&result->buf, ":");
                    {
                        char num[32];
                        snprintf(num, sizeof(num), "%d\n", file_matches);
                        buffer_append_str(&result->buf, num);
                    }
                    break;

                case OUTPUT_CONTENT:
                default:
                    buffer_append_str(&result->buf, filepath);
                    buffer_append_str(&result->buf, ":");
                    {
                        char num[32];
                        snprintf(num, sizeof(num), "%d: ", line_num);
                        buffer_append_str(&result->buf, num);
                    }
                    buffer_append_str(&result->buf, line);
                    buffer_append_str(&result->buf, "\n");
                    break;
            }

            result->total_matches++;
            result->files_matched++;
        }

        // Check max results limit
        if (cfg->max_results > 0 && result->total_matches >= cfg->max_results) {
            break;
        }
    }

    fclose(f);
}

// Walk directory iteratively (no recursion - avoids stack overflow)
typedef struct DirEntry {
    char path[MAX_PATH];
    struct DirEntry *next;
} DirEntry;

static int should_skip_dir(const char *dirname) {
    if (strcmp(dirname, ".git") == 0 || strcmp(dirname, "node_modules") == 0 ||
        strcmp(dirname, "__pycache__") == 0 || strcmp(dirname, ".cache") == 0 ||
        strcmp(dirname, ".claude") == 0 || strcmp(dirname, "dist") == 0 ||
        strcmp(dirname, "target") == 0 || strcmp(dirname, "build") == 0) {
        return 1;
    }
    return 0;
}

static void walk_directory(const char *start_dir, RGrepConfig *cfg, RGrepResult *result) {
    DirEntry *stack = NULL;

    DirEntry *first = calloc(1, sizeof(DirEntry));
    if (!first) return;
    strncpy(first->path, start_dir, MAX_PATH - 1);
    first->next = NULL;
    stack = first;

    while (stack && !is_output_full(result)) {
        DirEntry *current = stack;
        stack = stack->next;
        char dir[MAX_PATH];
        strncpy(dir, current->path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = '\0';
        free(current);

        // Skip known large directories
        const char *bname = strrchr(dir, '\\');
        if (!bname) bname = strrchr(dir, '/');
        if (!bname) bname = dir; else bname++;
        if (should_skip_dir(bname)) continue;

        char search_path[MAX_PATH];
        snprintf(search_path, sizeof(search_path), "%s\\*", dir);

        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(search_path, &fd);

        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (is_output_full(result)) break;

            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;

            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!should_skip_dir(fd.cFileName)) {
                    DirEntry *sub = calloc(1, sizeof(DirEntry));
                    if (sub) {
                        strncpy(sub->path, full_path, MAX_PATH - 1);
                        sub->next = stack;
                        stack = sub;
                    }
                }
            } else {
                if (should_search_file(full_path, cfg)) {
                    result->files_scanned++;
                    search_file(full_path, cfg, result);
                }
            }
        } while (FindNextFileA(h, &fd) && !is_output_full(result));

        FindClose(h);
    }

    while (stack) {
        DirEntry *next = stack->next;
        free(stack);
        stack = next;
    }
}

// Main search function
RGrepResult *rgrep_search(RGrepConfig *cfg) {
    if (!cfg || !cfg->pattern) {
        return NULL;
    }

    RGrepResult *result = calloc(1, sizeof(RGrepResult));
    if (!result) return NULL;

    buffer_init(&result->buf);
    result->files_scanned = 0;
    result->files_matched = 0;
    result->total_matches = 0;

    const char *search_path = cfg->path ? cfg->path : ".";
    if (!search_path || !search_path[0]) {
        search_path = ".";
    }

    DWORD attr = GetFileAttributesA(search_path);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (should_search_file(search_path, cfg)) {
            result->files_scanned = 1;
            search_file(search_path, cfg, result);
        }
    } else {
        walk_directory(search_path, cfg, result);
    }

    return result;
}

void rgrep_free_result(RGrepResult *result) {
    if (!result) return;
    buffer_free(&result->buf);
    free(result);
}

char *rgrep_get_output(RGrepResult *result) {
    if (!result || !result->buf.data) return strdup("");
    // Steal the buffer's data (caller takes ownership)
    char *data = result->buf.data;
    result->buf.data = NULL;
    result->buf.len = 0;
    result->buf.capacity = 0;
    return data;
}
