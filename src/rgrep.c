#include "rgrep.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
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
    size_t filename_len = strlen(filename);
    
    for (int i = 0; g_file_types[i][0]; i++) {
        if (strcmp(g_file_types[i][0], type) == 0) {
            const char *ext = g_file_types[i][1];
            // Check if it's a full filename (like Makefile, Dockerfile)
            if (ext[0] && isupper(ext[0])) {
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

// Compile regex pattern (simple regex support for common patterns)
static int regex_matches(const char *text, const char *pattern, int case_sensitive) {
    // If no special regex chars, use simple strstr
    const char *p = pattern;
    int has_special = 0;
    while (*p) {
        if (*p == '.' || *p == '*' || *p == '+' || *p == '?' || 
            *p == '[' || *p == '(' || *p == '|' || *p == '^' || *p == '$') {
            has_special = 1;
            break;
        }
        p++;
    }
    
    if (!has_special) {
        // Simple substring match
        if (case_sensitive) {
            return strstr(text, pattern) != NULL;
        } else {
            // Case-insensitive
            const char *t = text;
            while (*t) {
                const char *p1 = pattern;
                const char *t1 = t;
                while (*p1 && *t1 && 
                       tolower((unsigned char)*p1) == tolower((unsigned char)*t1)) {
                    p1++;
                    t1++;
                }
                if (*p1 == '\0') return 1;
                t++;
            }
            return 0;
        }
    }
    
    // Basic regex: . * ? ^ $ [abc] support
    // Compile pattern to simple regex state machine
    size_t plen = strlen(pattern);
    int *matched = malloc((strlen(text) + 1) * sizeof(int));
    if (!matched) return 0;
    
    for (size_t i = 0; i <= strlen(text); i++) matched[i] = 0;
    matched[0] = 1;
    
    size_t pi = 0;
    int group_start = -1;
    
    while (pi < plen) {
        char c = pattern[pi];
        int next_is_literal = 0;
        
        // Handle escape sequences
        if (c == '\\' && pi + 1 < plen) {
            c = pattern[pi + 1];
            next_is_literal = 1;
            pi++;
        }
        
        // Reset matched array for next position
        for (size_t i = 1; i <= strlen(text); i++) matched[i] = 0;
        
        switch (c) {
            case '^':
                // Start of line anchor - matched[0] stays true, others false
                break;
                
            case '$':
                // End of line anchor
                break;
                
            case '.':
                // Any character
                for (size_t i = 0; i < strlen(text); i++) {
                    matched[i] = matched[i] || matched[i + 1];
                }
                break;
                
            case '*':
                // Zero or more of previous
                // Already handled in next iteration
                break;
                
            case '+':
                // One or more of previous
                break;
                
            case '?':
                // Zero or one of previous
                break;
                
            case '[': {
                // Character class
                int negated = 0;
                pi++;
                if (pattern[pi] == '^') {
                    negated = 1;
                    pi++;
                }
                // Find closing bracket
                size_t end = pi;
                while (end < plen && pattern[end] != ']') end++;
                
                for (size_t i = 0; i < strlen(text); i++) {
                    int in_class = 0;
                    for (size_t j = pi; j < end; j++) {
                        char cc = pattern[j];
                        if (pattern[j + 1] == '-' && j + 2 < end) {
                            // Range
                            char start = pattern[j];
                            char endc = pattern[j + 2];
                            if (case_sensitive) {
                                if (text[i] >= start && text[i] <= endc) in_class = 1;
                            } else {
                                if (tolower((unsigned char)text[i]) >= tolower((unsigned char)start) &&
                                    tolower((unsigned char)text[i]) <= tolower((unsigned char)endc)) {
                                    in_class = 1;
                                }
                            }
                            j += 2;
                        } else {
                            if (case_sensitive) {
                                if (text[i] == cc) in_class = 1;
                            } else {
                                if (tolower((unsigned char)text[i]) == tolower((unsigned char)cc)) {
                                    in_class = 1;
                                }
                            }
                        }
                    }
                    if (negated ? !in_class : in_class) {
                        matched[i] = matched[i] || matched[i + 1];
                    }
                }
                pi = end;
                break;
            }
            
            case '(':
                group_start = pi;
                break;
                
            case ')':
                group_start = -1;
                break;
                
            case '|':
                // Alternation - reset and try from start
                break;
                
            default:
                // Literal character
                for (size_t i = 0; i < strlen(text); i++) {
                    if (case_sensitive) {
                        if (text[i] == c) matched[i] = matched[i] || matched[i + 1];
                    } else {
                        if (tolower((unsigned char)text[i]) == tolower((unsigned char)c)) {
                            matched[i] = matched[i] || matched[i + 1];
                        }
                    }
                }
                break;
        }
        
        // Handle * after character
        if (pi + 1 < plen && pattern[pi + 1] == '*') {
            // Zero or more - already handled by loop
            pi++;
        }
        
        pi++;
    }
    
    // Check if any position is matched
    int result = 0;
    for (size_t i = 0; i <= strlen(text); i++) {
        if (matched[i]) {
            result = 1;
            break;
        }
    }
    
    free(matched);
    return result;
}

// Search a single file
static void search_file(const char *filepath, RGrepConfig *cfg, RGrepResult *result) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;
    
    char line[8192];
    int line_num = 0;
    int file_matches = 0;
    
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        
        // Truncate line if too long
        if (cfg->max_line_length > 0 && (int)strlen(line) > cfg->max_line_length) {
            line[cfg->max_line_length] = '\0';
        }
        
        // Remove trailing newline for matching
        size_t line_len = strlen(line);
        int has_newline = 0;
        if (line_len > 0 && line[line_len - 1] == '\n') {
            has_newline = 1;
            line[line_len - 1] = '\0';
            line_len--;
        }
        
        // Check for match
        if (regex_matches(line, cfg->pattern, cfg->case_sensitive)) {
            file_matches++;
            
            // Check max count per file
            if (cfg->max_count > 0 && file_matches > cfg->max_count) {
                break;
            }
            
            // Format based on output mode
            switch (cfg->output_mode) {
                case OUTPUT_FILES:
                    // Just add filepath
                    buffer_append_str(&result->buf, filepath);
                    buffer_append_str(&result->buf, "\n");
                    break;
                    
                case OUTPUT_COUNT:
                    // File:count format
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
                    // path:line:content format
                    buffer_append_str(&result->buf, filepath);
                    buffer_append_str(&result->buf, ":");
                    {
                        char num[32];
                        snprintf(num, sizeof(num), "%d: ", line_num);
                        buffer_append_str(&result->buf, num);
                    }
                    buffer_append_str(&result->buf, line);
                    if (has_newline) buffer_append_str(&result->buf, "\n");
                    else buffer_append_str(&result->buf, "\n");
                    break;
            }
            
            result->total_matches++;
            result->files_matched++;
        }
    }
    
    fclose(f);
}

// Walk directory recursively
static void walk_directory(const char *dir, RGrepConfig *cfg, RGrepResult *result) {
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
        
        // Build full path
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fd.cFileName);
        
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse into subdirectory
            walk_directory(full_path, cfg, result);
        } else {
            // Check file filters
            if (should_search_file(full_path, cfg)) {
                result->files_scanned++;
                
                // Check max results
                if (cfg->max_results > 0 && result->total_matches >= cfg->max_results) {
                    break;
                }
                
                search_file(full_path, cfg, result);
            }
        }
    } while (FindNextFileA(h, &fd) && (cfg->max_results == 0 || result->total_matches < cfg->max_results));
    
    FindClose(h);
}

// Main search function
RGrepResult *rgrep_search(RGrepConfig *cfg) {
    RGrepResult *result = calloc(1, sizeof(RGrepResult));
    if (!result) return NULL;
    
    buffer_init(&result->buf);
    result->files_scanned = 0;
    result->files_matched = 0;
    result->total_matches = 0;
    
    // Check if path is a file
    DWORD attr = GetFileAttributesA(cfg->path);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // It's a file
        if (should_search_file(cfg->path, cfg)) {
            result->files_scanned = 1;
            search_file(cfg->path, cfg, result);
        }
    } else {
        // It's a directory
        walk_directory(cfg->path, cfg, result);
    }
    
    return result;
}

void rgrep_free_result(RGrepResult *result) {
    if (!result) return;
    buffer_free(&result->buf);
    free(result);
}

char *rgrep_get_output(RGrepResult *result) {
    if (!result) return strdup("");
    return buffer_c_str(&result->buf);
}
