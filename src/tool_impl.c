#include "tool.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <windows.h>

// Forward declarations for functions defined later in the file
static char *normalize_line_endings(const char *text);
static char *restore_line_endings(const char *text, int use_crlf);
static int fuzzy_find_text(const char *content, const char *search);
static double eval_expr(const char *expr);

// LCS-based diff generation for EditDiff
static char **compute_lcs(const char **a, int a_len, const char **b, int b_len, int *lcs_len);
static char *generate_unified_diff(const char *filename, const char *old_content, const char *new_content, int context_lines);

// ============== Shell Security: Deny Patterns ==============
static const char *g_deny_patterns[] = {
    "\\brm\\s+-[rf]{1,2}\\b",
    "\\bdel\\s+/[fq]\\b",
    "\\brmdir\\s+/s\\b",
    "(?:^|[;&|]\\s*)format\\b",
    "\\b(mkfs|diskpart)\\b",
    "\\bdd\\s+.*\\bof=",
    ">\\s*/dev/sd",
    "\\b(shutdown|reboot|poweroff)\\b",
    ":\\(\\)\\s*\\{.*\\};\\s*:",
    "\\w+\\(\\)\\s*\\{[^}]*\\|\\s*[^}]*&\\s*\\}\\s*;\\s*",
    "remove-item\\s",
    "\\bri\\s+",
    "remove-itemproperty\\s",
    "rd\\s+/[sS]\\b",
    "docker\\s+system\\s+prune",
    "docker\\s+\\S+\\s+prune",
    "git\\s+push\\s+.*--force",
    "git\\s+push\\s+-f\\b",
    "git\\s+clean\\s+-[fd]",
    "git\\s+reset\\s+--hard",
    "git\\s+checkout\\s+--force",
    "git\\s+rebase\\s+--interactive",
    "git\\s+filter-branch",
    "git\\s+reflog\\s+expire",
    "&\\S*&\\S*&",
};

static int pattern_match(const char *pattern, const char *text) {
    // Simple substring match with word boundaries
    const char *p = pattern;
    int in_word = 0;
    
    while (*text) {
        if (*text == '\\') {
            // Handle word boundary markers
            if (text[1] == 'b') {
                // Word boundary
                int before = in_word;
                text += 2;
                // Check what comes before
                if (before && isalnum((unsigned char)text[0])) {
                    continue; // Not at word boundary
                }
                p = pattern;
                in_word = 0;
                continue;
            }
        }
        
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'b') {
                p++;
                continue;
            }
        }
        
        if (*p == *text || (*p == '.' && *text)) {
            p++;
            if (*p == '\0') return 1;
        } else {
            p = pattern;
            if (*p == *text || (*p == '.' && *text)) {
                p++;
            }
        }
        text++;
    }
    return 0;
}

// Check if command contains dangerous patterns
static int check_deny_patterns(const char *cmd) {
    if (!cmd) return 0;
    
    // Case-insensitive check
    char *lower = strdup(cmd);
    if (!lower) return 0;
    
    for (char *p = lower; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    
    for (size_t i = 0; i < sizeof(g_deny_patterns) / sizeof(g_deny_patterns[0]); i++) {
        const char *pat = g_deny_patterns[i];
        if (pattern_match(pat, lower)) {
            free(lower);
            return 1;
        }
    }
    
    free(lower);
    return 0;
}

// ============== Interactive Prompt Detection ==============
static const char *g_interactive_patterns[] = {
    "\\(y/n\\)",
    "\\[y/n\\]",
    "\\(yes/no\\)",
    "press (any key|enter)",
    "continue\\?",
    "overwrite\\?",
    "password:.*",
    "passphrase:.*",
    "enter.*password",
    "sudo:.*password",
    ">>> $",
    "In \\[\\d+\\]:",
    "\\.\\.\\. $",
};

static const char *match_interactive_prompt(const char *text) {
    if (!text) return NULL;
    
    size_t len = strlen(text);
    // Check last 1024 chars
    if (len > 1024) text = text + len - 1024;
    
    for (size_t i = 0; i < sizeof(g_interactive_patterns) / sizeof(g_interactive_patterns[0]); i++) {
        if (strstr(text, g_interactive_patterns[i])) {
            return g_interactive_patterns[i];
        }
    }
    return NULL;
}

// ============== Millisecond timer ==============
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Convert GBK (CP936) output from bash to UTF-8 for proper Chinese display
static char *convert_gbk_to_utf8(const char *input) {
    if (!input) return NULL;

    // First convert GBK to UTF-16
    int wlen = MultiByteToWideChar(936, 0, input, -1, NULL, 0);
    if (wlen == 0) return strdup(input);  // Fallback: return as-is

    wchar_t *wbuf = malloc(wlen * sizeof(wchar_t));
    if (!wbuf) return strdup(input);

    MultiByteToWideChar(936, 0, input, -1, wbuf, wlen);

    // Then convert UTF-16 to UTF-8
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
    if (ulen == 0) {
        free(wbuf);
        return strdup(input);
    }

    char *result = malloc(ulen);
    if (result) {
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, result, ulen, NULL, NULL);
    }
    free(wbuf);
    return result ? result : strdup(input);
}
#include <math.h>
#include <windows.h>
#include <io.h>

// UTF-8 to wide string converter for file paths
static wchar_t *utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t *result = malloc(size * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result, size);
    return result;
}

// UTF-8 fopen wrapper
static FILE *utf8_fopen(const char *path, const char *mode) {
    wchar_t *w_path = utf8_to_wide(path);
    wchar_t *w_mode = utf8_to_wide(mode);
    if (!w_path || !w_mode) {
        free(w_path);
        free(w_mode);
        return fopen(path, mode);  // Fallback
    }
    FILE *f = _wfopen(w_path, w_mode);
    free(w_path);
    free(w_mode);
    return f;
}

// Forward declaration for eval_expr
static double eval_expr(const char *expr);

// Escape string for JSON (reserved for future use)
// static char *json_escape(const char *str) { ... }

// Built-in tool implementations
// Max output size for Read tool (50KB)
#define MAX_READ_OUTPUT 51200

// Helper: split text into lines (returns NULL-terminated array)
static char **split_text_lines(const char *text, int *count) {
    if (!text) { *count = 0; return NULL; }
    int cap = 128;
    char **lines = malloc(cap * sizeof(char*));
    if (!lines) return NULL;
    int n = 0;
    const char *start = text;
    while (*text) {
        if (*text == '\n') {
            if (n >= cap - 1) {
                cap *= 2;
                char **tmp = realloc(lines, cap * sizeof(char*));
                if (!tmp) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
                lines = tmp;
            }
            size_t len = text - start;
            lines[n] = malloc(len + 1);
            if (!lines[n]) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
            memcpy(lines[n], start, len);
            lines[n][len] = '\0';
            n++;
            start = text + 1;
        }
        text++;
    }
    // Last line (no trailing newline)
    if (*start) {
        if (n >= cap - 1) {
            cap *= 2;
            char **tmp = realloc(lines, cap * sizeof(char*));
            if (!tmp) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
            lines = tmp;
        }
        lines[n] = strdup(start);
        if (!lines[n]) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
        n++;
    }
    *count = n;
    return lines;
}

char *tool_read_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }
    
    cJSON *offset = cJSON_GetObjectItem(input, "offset");
    cJSON *limit = cJSON_GetObjectItem(input, "limit");
    
    // offset: 1-indexed line number (0 means no offset)
    int line_offset = 0;
    // limit: max lines to read (0 means default 500)
    int line_limit = 0;
    
    if (offset && offset->type == cJSON_Number) {
        int val = (int)offset->valuedouble;
        if (val < 1) {
            *error = strdup("offset must be >= 1");
            return NULL;
        }
        line_offset = val - 1; // convert to 0-indexed
    }
    if (limit && limit->type == cJSON_Number) {
        line_limit = (int)limit->valuedouble;
    }
    if (line_limit == 0) line_limit = 500; // default
    
    FILE *f = utf8_fopen(path->valuestring, "rb");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *file_content = malloc(size + 1);
    if (!file_content) {
        fclose(f);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    long read = fread(file_content, 1, size, f);
    file_content[read] = '\0';
    fclose(f);
    
    // Strip UTF-8 BOM if present
    char *content = file_content;
    if (size >= 3 && (unsigned char)file_content[0] == 0xEF && 
        (unsigned char)file_content[1] == 0xBB && (unsigned char)file_content[2] == 0xBF) {
        content = file_content + 3;
    }
    
    // Split into lines
    int total_lines = 0;
    char **all_lines = split_text_lines(content, &total_lines);
    if (!all_lines) {
        free(file_content);
        *error = strdup("failed to split file into lines");
        return NULL;
    }
    
    // Check offset bounds
    if (line_offset >= total_lines) {
        for (int i = 0; i < total_lines; i++) free(all_lines[i]);
        free(all_lines);
        free(file_content);
        *error = strdup("offset is beyond end of file");
        return NULL;
    }
    
    // Apply offset
    int start_line = line_offset;
    int remaining = total_lines - start_line;
    
    // Apply limit
    int count = line_limit;
    if (count <= 0 || count > remaining) {
        count = remaining;
    }
    
    // Extract lines
    char **selected = all_lines + start_line;
    int selected_count = count;
    
    // Join selected lines
    Buffer result_buf;
    buffer_init(&result_buf);
    for (int i = 0; i < selected_count; i++) {
        if (i > 0) buffer_append_str(&result_buf, "\n");
        buffer_append_str(&result_buf, selected[i]);
    }
    char *result = buffer_c_str(&result_buf);
    buffer_free(&result_buf);
    
    // Free all_lines
    for (int i = 0; i < total_lines; i++) free(all_lines[i]);
    free(all_lines);
    free(file_content);
    
    // Build suffix for pagination info
    Buffer out_buf;
    buffer_init(&out_buf);
    buffer_append_str(&out_buf, result);
    free(result);
    
    int next_offset = start_line + selected_count;
    int more_lines = total_lines - next_offset;
    
    if (more_lines > 0) {
        char suffix[256];
        snprintf(suffix, sizeof(suffix), "\n\n[%d more lines in file. Use offset=%d to continue.]", 
                 more_lines, next_offset + 1);
        buffer_append_str(&out_buf, suffix);
    }
    
    char *output = buffer_c_str(&out_buf);
    buffer_free(&out_buf);
    
    // Truncate to MAX_READ_OUTPUT if needed
    size_t out_len = strlen(output);
    if (out_len > MAX_READ_OUTPUT) {
        output[MAX_READ_OUTPUT] = '\0';
        // Try to append truncation notice
        Buffer trunc_buf;
        buffer_init(&trunc_buf);
        buffer_append(&trunc_buf, output, MAX_READ_OUTPUT);
        buffer_append_str(&trunc_buf, "\n\n[Output truncated to 50KB.]");
        free(output);
        output = buffer_c_str(&trunc_buf);
        buffer_free(&trunc_buf);
    }
    
    return output;
}

char *tool_write_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }
    if (!content || !content->valuestring) {
        *error = strdup("missing content parameter");
        return NULL;
    }
    
    // Create parent directories if they don't exist
    wchar_t *w_path = utf8_to_wide(path->valuestring);
    if (!w_path) {
        *error = strdup("failed to convert path to wide string");
        return NULL;
    }
    
    // Get directory path
    wchar_t *dir_path = _wcsdup(w_path);
    if (!dir_path) {
        free(w_path);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    // Find last backslash or forward slash
    wchar_t *last_sep = wcsrchr(dir_path, L'\\');
    wchar_t *last_fwd = wcsrchr(dir_path, L'/');
    wchar_t *last = last_sep;
    if (!last || (last_fwd && last_fwd > last)) last = last_fwd;
    
    if (last) {
        *last = L'\0';
        // Create directory tree
        if (!CreateDirectoryW(dir_path, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                free(w_path);
                free(dir_path);
                *error = strdup("failed to create directory");
                return NULL;
            }
        }
    }
    free(dir_path);
    
    FILE *f = _wfopen(w_path, L"w");
    free(w_path);
    
    if (!f) {
        *error = strdup("failed to open file for writing");
        return NULL;
    }
    
    fwrite(content->valuestring, 1, strlen(content->valuestring), f);
    fclose(f);
    
    char *result;
    size_t content_len = strlen(content->valuestring);
    result = malloc(64 + strlen(path->valuestring) + 20);
    if (result) {
        sprintf(result, "Successfully wrote %zu bytes to %s", content_len, path->valuestring);
    } else {
        result = strdup("File written successfully");
    }
    return result;
}

char *tool_edit_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *old_str = cJSON_GetObjectItem(input, "old_string");
    cJSON *new_str = cJSON_GetObjectItem(input, "new_string");
    
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }
    if (!old_str || !old_str->valuestring) {
        *error = strdup("missing old_string parameter");
        return NULL;
    }
    if (!new_str || !new_str->valuestring) {
        *error = strdup("missing new_string parameter");
        return NULL;
    }
    
    // Read file content
    FILE *f = utf8_fopen(path->valuestring, "r");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    // Strip UTF-8 BOM if present
    char *file_content = content;
    int bom_len = 0;
    if (size >= 3 && (unsigned char)content[0] == 0xEF && 
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        bom_len = 3;
        file_content = content + 3;
    }
    
    // Detect line ending
    int has_crlf = 0;
    for (int i = 0; i < size - bom_len; i++) {
        if (content[i] == '\r' && i + 1 < size && content[i + 1] == '\n') {
            has_crlf = 1;
            break;
        }
    }
    
    // Normalize line endings to LF for matching
    char *normalized = normalize_line_endings(file_content);
    if (!normalized) {
        free(content);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    // Also normalize old_str and new_str for matching
    char *normalized_old = normalize_line_endings(old_str->valuestring);
    char *normalized_new = normalize_line_endings(new_str->valuestring);
    if (!normalized_old || !normalized_new) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    // Try exact match first, then fuzzy
    char *pos = strstr(normalized, normalized_old);
    int match_len = 0;
    
    if (!pos) {
        // Try fuzzy match
        int fuzzy_idx = fuzzy_find_text(normalized, normalized_old);
        if (fuzzy_idx >= 0) {
            pos = normalized + fuzzy_idx;
        }
    }
    
    if (pos) {
        match_len = strlen(normalized_old);
    }
    
    if (!pos) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        *error = strdup("Could not find the exact text in file. The old text must match exactly including all whitespace and newlines.");
        return NULL;
    }
    
    // Check for unique occurrence
    int occurrences = 0;
    char *check_pos = normalized;
    while ((check_pos = strstr(check_pos, normalized_old)) != NULL) {
        occurrences++;
        check_pos++;
    }
    if (occurrences > 1) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "Found %d occurrences of the text. The text must be unique.", occurrences);
        *error = strdup(err_buf);
        return NULL;
    }
    
    // Calculate positions in original content
    int match_offset = pos - normalized;
    
    // Build new content: before match + new text + after match
    int new_size = (pos - normalized) + strlen(normalized_new) + (strlen(normalized) - match_offset - match_len);
    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    memcpy(new_content, normalized, match_offset);
    memcpy(new_content + match_offset, normalized_new, strlen(normalized_new));
    memcpy(new_content + match_offset + strlen(normalized_new), 
           normalized + match_offset + match_len, 
           strlen(normalized) - match_offset - match_len);
    new_content[new_size] = '\0';
    
    // Restore original line endings
    if (has_crlf) {
        char *restored = restore_line_endings(new_content, 1);  // 1 = CRLF
        free(new_content);
        if (!restored) {
            free(content);
            free(normalized);
            free(normalized_old);
            free(normalized_new);
            *error = strdup("memory allocation failed");
            return NULL;
        }
        new_content = restored;
    }
    
    // Check if content changed
    if (strcmp(file_content, new_content) == 0) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        free(new_content);
        *error = strdup("No changes made. The replacement produced identical content.");
        return NULL;
    }
    
    // Write back with BOM if originally present
    f = utf8_fopen(path->valuestring, "w");
    if (!f) {
        free(content);
        free(normalized);
        free(normalized_old);
        free(normalized_new);
        free(new_content);
        *error = strdup("failed to open file for writing");
        return NULL;
    }
    
    if (bom_len > 0) {
        fwrite("\xEF\xBB\xBF", 1, 3, f);
    }
    fwrite(new_content, 1, strlen(new_content), f);
    fclose(f);
    
    // Generate unified diff for display (with 10 lines of context)
    char *diff = generate_unified_diff(path->valuestring, file_content, new_content, 16);
    
    free(content);
    free(normalized);
    free(normalized_old);
    free(normalized_new);
    free(new_content);
    
    if (diff) {
        return diff;
    }
    return strdup("File edited successfully");
}

// Normalize line endings to LF only
static char *normalize_line_endings(const char *text) {
    if (!text) return NULL;
    
    int len = strlen(text);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\r') {
            if (i + 1 < len && text[i + 1] == '\n') {
                // CRLF -> LF
                result[j++] = '\n';
                i++;
            } else {
                // Lone CR -> LF
                result[j++] = '\n';
            }
        } else {
            result[j++] = text[i];
        }
    }
    result[j] = '\0';
    return result;
}

// Restore line endings from LF to specified format
static char *restore_line_endings(const char *text, int use_crlf) {
    if (!text) return NULL;
    
    int len = strlen(text);
    int count_lf = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') count_lf++;
    }
    
    int new_len = len + (use_crlf ? count_lf : 0);
    char *result = malloc(new_len + 1);
    if (!result) return NULL;
    
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n' && use_crlf) {
            result[j++] = '\r';
        }
        result[j++] = text[i];
    }
    result[j] = '\0';
    return result;
}

// Normalize text for fuzzy matching (lowercase)
static char *normalize_for_fuzzy(const char *text) {
    if (!text) return NULL;
    
    char *result = malloc(strlen(text) + 1);
    if (!result) return NULL;
    
    int i = 0;
    while (text[i]) {
        // Convert to lowercase
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z') {
            result[i] = c + 32;
        } else {
            result[i] = c;
        }
        i++;
    }
    result[i] = '\0';
    return result;
}

// Fuzzy find text (case-insensitive match)
static int fuzzy_find_text(const char *content, const char *search) {
    if (!content || !search) return -1;
    
    char *norm_content = normalize_for_fuzzy(content);
    char *norm_search = normalize_for_fuzzy(search);
    if (!norm_content || !norm_search) {
        free(norm_content);
        free(norm_search);
        return -1;
    }
    
    char *pos = strstr(norm_content, norm_search);
    int result = -1;
    if (pos) {
        result = pos - norm_content;
    }
    
    free(norm_content);
    free(norm_search);
    return result;
}

char *tool_grep(cJSON *input, char **error) {
    cJSON *pattern = cJSON_GetObjectItem(input, "pattern");
    cJSON *path = cJSON_GetObjectItem(input, "path");
    
    if (!pattern || !pattern->valuestring) {
        *error = strdup("missing pattern parameter");
        return NULL;
    }
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }
    
    FILE *f = utf8_fopen(path->valuestring, "r");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }
    
    Buffer buf;
    buffer_init(&buf);
    
    char line[4096];
    int line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        if (strstr(line, pattern->valuestring)) {
            char num_buf[32];
            snprintf(num_buf, sizeof(num_buf), "%d: ", line_num);
            buffer_append_str(&buf, num_buf);
            buffer_append_str(&buf, line);
        }
    }
    
    fclose(f);
    
    char *result = buffer_c_str(&buf);
    char *ret = strdup(result);
    buffer_free(&buf);
    
    return ret ? ret : strdup("");
}

char *tool_glob(cJSON *input, char **error) {
    cJSON *pat = cJSON_GetObjectItem(input, "pattern");
    
    if (!pat || !pat->valuestring) {
        *error = strdup("missing pattern parameter");
        return NULL;
    }
    
    Buffer buf;
    buffer_init(&buf);
    
    // Parse pattern - support **/*.ext and *.ext formats
    char *pattern = strdup(pat->valuestring);
    if (!pattern) {
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    char *dir = ".";
    char *file_pat = pattern;
    
    // Find last slash
    char *last_slash = strrchr(pattern, '/');
    char *last_bslash = strrchr(pattern, '\\');
    if (last_bslash && (!last_slash || last_bslash > last_slash)) {
        last_slash = last_bslash;
    }
    
    if (last_slash) {
        *last_slash = '\0';
        dir = pattern;
        file_pat = last_slash + 1;
    }
    
    // Build search path
    char search_path[MAX_PATH];
    if (strchr(file_pat, '*') || strchr(file_pat, '?')) {
        // Has wildcards
        snprintf(search_path, sizeof(search_path), "%s\\%s", dir, file_pat);
    } else {
        // No wildcards, add *
        snprintf(search_path, sizeof(search_path), "%s\\%s*", dir, file_pat);
    }
    
    WIN32_FIND_DATA FindFileData;
    HANDLE hFind = FindFirstFileA(search_path, &FindFileData);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(FindFileData.cFileName, ".") != 0 && 
                strcmp(FindFileData.cFileName, "..") != 0) {
                // Build path safely
                buffer_append_str(&buf, dir);
                buffer_append(&buf, "\\", 1);
                buffer_append_str(&buf, FindFileData.cFileName);
                buffer_append(&buf, "\n", 1);
            }
        } while (FindNextFileA(hFind, &FindFileData));
        FindClose(hFind);
    }
    
    free(pattern);
    
    char *result = buffer_c_str(&buf);
    char *ret = strdup(result);
    buffer_free(&buf);
    
    return ret ? ret : strdup("");
}

char *tool_exec(cJSON *input, char **error) {
    cJSON *cmd = cJSON_GetObjectItem(input, "command");
    cJSON *cwd = cJSON_GetObjectItem(input, "cwd");
    cJSON *timeout = cJSON_GetObjectItem(input, "timeout");

    if (!cmd || !cmd->valuestring) {
        *error = strdup("missing command parameter");
        return NULL;
    }

    const char *command = cmd->valuestring;
    const char *work_dir = cwd && cwd->valuestring ? cwd->valuestring : NULL;
    int timeout_sec = 120;
    if (timeout && timeout->type == cJSON_Number) {
        timeout_sec = (int)timeout->valuedouble;
        if (timeout_sec <= 0) timeout_sec = 120;
        if (timeout_sec > 600) timeout_sec = 600;
    }

    // Security check
    if (check_deny_patterns(command)) {
        *error = strdup("dangerous command pattern detected");
        return NULL;
    }

    // Escape single quotes in command for bash -c '...'
    int cmdlen = strlen(command);
    char *escaped = malloc(cmdlen * 4 + 1);
    if (!escaped) {
        *error = strdup("out of memory");
        return NULL;
    }
    int j = 0;
    for (int i = 0; i < cmdlen; i++) {
        if (command[i] == '\'') {
            escaped[j++] = '\'';
            escaped[j++] = '\\';
            escaped[j++] = '\'';
            escaped[j++] = '\'';
        } else {
            escaped[j++] = command[i];
        }
    }
    escaped[j] = '\0';

    // Find bash executable
    const char *bash_paths[] = {
        "E:/Git/bin/bash.exe",
        "C:/Program Files/Git/bin/bash.exe",
        "C:/Program Files (x86)/Git/bin/bash.exe",
        "bash",
        NULL
    };

    const char *bash_exe = NULL;
    for (int i = 0; bash_paths[i]; i++) {
        if (GetFileAttributesA(bash_paths[i]) != INVALID_FILE_ATTRIBUTES) {
            bash_exe = bash_paths[i];
            break;
        }
    }
    if (!bash_exe) {
        free(escaped);
        *error = strdup("bash not found for command execution");
        return NULL;
    }

    // Build command line: "bash" -c 'command' or with cd prefix
    char cmd_line[8192];
    if (work_dir && strlen(work_dir) > 0) {
        // Convert Windows path to POSIX for bash
        char posix_dir[512];
        int pi = 0;
        for (int ci = 0; work_dir[ci] && pi < (int)(sizeof(posix_dir) - 1); ci++) {
            if (work_dir[ci] == '\\') {
                posix_dir[pi++] = '/';
            } else if (work_dir[ci] == ':') {
                // Drive letter: C: -> /c
                posix_dir[pi++] = '/';
                posix_dir[pi++] = tolower((unsigned char)work_dir[++ci]);
            } else {
                posix_dir[pi++] = tolower((unsigned char)work_dir[ci]);
            }
        }
        posix_dir[pi] = '\0';
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c 'cd %s && %s'", bash_exe, posix_dir, escaped);
    } else {
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c '%s'", bash_exe, escaped);
    }
    free(escaped);

    // Create pipes for stdout and stderr
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // Create stdout pipe
    if (!CreatePipe(&hReadOut, &hWriteOut, &sa, 0)) {
        *error = strdup("failed to create stdout pipe");
        return NULL;
    }
    SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);

    // Create stderr pipe
    if (!CreatePipe(&hReadErr, &hWriteErr, &sa, 0)) {
        CloseHandle(hReadOut);
        CloseHandle(hWriteOut);
        *error = strdup("failed to create stderr pipe");
        return NULL;
    }
    SetHandleInformation(hReadErr, HANDLE_FLAG_INHERIT, 0);

    si.hStdOutput = hWriteOut;
    si.hStdError = hWriteErr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                        NULL, work_dir, &si, &pi)) {
        CloseHandle(hReadOut);
        CloseHandle(hWriteOut);
        CloseHandle(hReadErr);
        CloseHandle(hWriteErr);
        *error = strdup("failed to create process");
        return NULL;
    }

    CloseHandle(hWriteOut);  // Close write end in parent (stdout)
    CloseHandle(hWriteErr);  // Close write end in parent (stderr)

    // Read output with stall detection
    Buffer buf;
    buffer_init(&buf);
    char chunk[4096];
    DWORD bytes_read;
    
    long long start_time = get_time_ms();
    long long last_activity = start_time;
    int stall_threshold_ms = (timeout_sec * 1000) / 3;
    if (stall_threshold_ms < 10000) stall_threshold_ms = 10000;
    
    int running = 1;
    int stdout_closed = 0;
    int stderr_closed = 0;
    
    while (running) {
        // Check timeout
        long long now = get_time_ms();
        if (now - start_time > timeout_sec * 1000) {
            // Kill the process
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
            Sleep(100);
            TerminateProcess(pi.hProcess, 1);
            buffer_append_str(&buf, "\n[Command timed out]\n");
            break;
        }
        
        // Check for output on stdout
        if (!stdout_closed) {
            if (ReadFile(hReadOut, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                chunk[bytes_read] = '\0';
                buffer_append_str(&buf, chunk);
                last_activity = get_time_ms();
            } else {
                stdout_closed = 1;
            }
        }
        
        // Check for output on stderr
        if (!stderr_closed) {
            if (ReadFile(hReadErr, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                chunk[bytes_read] = '\0';
                buffer_append_str(&buf, chunk);
                last_activity = get_time_ms();
            } else {
                stderr_closed = 1;
            }
        }
        
        // Check if both pipes closed and process might have exited
        if (stdout_closed && stderr_closed) {
            DWORD exitCode;
            if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
                if (exitCode != STILL_ACTIVE) {
                    running = 0;
                    break;
                }
            }
        }
        
        // Stall detection: check if command is waiting for input
        now = get_time_ms();
        if (!stdout_closed && !stderr_closed && (now - last_activity) > stall_threshold_ms) {
            // Check if output contains interactive prompts
            const char *tail = buffer_c_str(&buf);
            if (tail && match_interactive_prompt(tail)) {
                // Kill the process (don't close handles here, will do it after loop)
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
                Sleep(100);
                TerminateProcess(pi.hProcess, 1);
                stdout_closed = 1;
                stderr_closed = 1;
                running = 0;  // Signal loop to exit
                const char *pattern = match_interactive_prompt(tail);
                snprintf(cmd_line, sizeof(cmd_line), 
                    "\n[Command may be waiting for input. Matched pattern: %s]\n"
                    "This usually means the command needs stdin. Use non-interactive flags instead.",
                    pattern ? pattern : "stall");
                buffer_append_str(&buf, cmd_line);
                break;
            }
            last_activity = now;  // Reset to avoid repeated detection
        }
        
        // Small sleep to avoid busy-waiting
        Sleep(10);
    }

    // Wait for process to finish (only if still running)
    if (running) {
        WaitForSingleObject(pi.hProcess, 1000);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadOut);
    CloseHandle(hReadErr);

    // Get exit code
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        exitCode = 1;
    }

    // Convert GBK output to UTF-8 for proper Chinese display
    char *result = buffer_c_str(&buf);
    char *utf8_result = convert_gbk_to_utf8(result);
    buffer_free(&buf);

    // Append exit code if not 0
    char *final_result;
    if (exitCode != 0) {
        final_result = malloc(strlen(utf8_result) + 64);
        if (final_result) {
            snprintf(final_result, strlen(utf8_result) + 64, "%s\nExit code: %lu", utf8_result, (unsigned long)exitCode);
            free(utf8_result);
            return final_result;
        }
    }

    return utf8_result ? utf8_result : strdup("");
}

char *tool_calc(cJSON *input, char **error) {
    cJSON *expr = cJSON_GetObjectItem(input, "expression");
    
    if (!expr || !expr->valuestring) {
        *error = strdup("missing expression parameter");
        return NULL;
    }
    
    double result = eval_expr(expr->valuestring);
    
    char *str = malloc(64);
    if (!str) {
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    // Check for special values
    if (isnan(result)) {
        strcpy(str, "NaN");
    } else if (isinf(result)) {
        strcpy(str, result > 0 ? "Infinity" : "-Infinity");
    } else {
        snprintf(str, 64, "%.10g", result);
    }
    
    return str;
}

// Simple expression evaluator with basic math operations
static double eval_expr(const char *expr) {
    double result = 0;
    double current = 0;
    char op = '+';
    const char *p = expr;
    
    while (*p) {
        // Skip whitespace
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        
        // Handle operators
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '^' || *p == '%') {
            // Apply previous operator
            switch (op) {
                case '+': result += current; break;
                case '-': result -= current; break;
                case '*': result *= current; break;
                case '/': 
                    if (current != 0) result /= current; 
                    else return NAN;
                    break;
                case '^': result = pow(result, current); break;
                case '%': 
                    if (current != 0) result = fmod(result, current);
                    else return NAN;
                    break;
            }
            op = *p;
            current = 0;
        } else {
            // Parse number
            char *end;
            current = strtod(p, &end);
            p = end - 1;
        }
        p++;
    }
    
    // Apply final operator
    switch (op) {
        case '+': result += current; break;
        case '-': result -= current; break;
        case '*': result *= current; break;
        case '/': 
            if (current != 0) result /= current; 
            else return NAN;
            break;
        case '^': result = pow(result, current); break;
        case '%': 
            if (current != 0) result = fmod(result, current);
            else return NAN;
            break;
    }
    
    return result;
}

// ============================================================================
// EditDiff: Unified diff generation for file edits
// ============================================================================

// Split content into lines (preserving trailing newlines)
static char **split_lines(const char *content, int *count) {
    if (!content) {
        *count = 0;
        return NULL;
    }
    
    int capacity = 64;
    char **lines = malloc(capacity * sizeof(char*));
    if (!lines) return NULL;
    
    int len = strlen(content);
    int line_count = 0;
    int start = 0;
    
    for (int i = 0; i <= len; i++) {
        if (i == len || content[i] == '\n') {
            int line_len = i - start;
            if (line_len > 0 && content[start + line_len - 1] == '\r') {
                line_len--;
            }
            
            char *line = malloc(line_len + 1);
            if (!line) {
                for (int j = 0; j < line_count; j++) free(lines[j]);
                free(lines);
                return NULL;
            }
            memcpy(line, content + start, line_len);
            line[line_len] = '\0';
            
            if (line_count >= capacity) {
                capacity *= 2;
                char **new_lines = realloc(lines, capacity * sizeof(char*));
                if (!new_lines) {
                    free(line);
                    for (int j = 0; j < line_count; j++) free(lines[j]);
                    free(lines);
                    return NULL;
                }
                lines = new_lines;
            }
            lines[line_count++] = line;
            start = i + 1;
        }
    }
    
    *count = line_count;
    return lines;
}

// Compute LCS (Longest Common Subsequence)
static char **compute_lcs(const char **a, int a_len, const char **b, int b_len, int *lcs_len) {
    if (a_len == 0 || b_len == 0) {
        *lcs_len = 0;
        return NULL;
    }
    
    int **dp = malloc((a_len + 1) * sizeof(int*));
    for (int i = 0; i <= a_len; i++) {
        dp[i] = calloc(b_len + 1, sizeof(int));
    }
    
    for (int i = 1; i <= a_len; i++) {
        for (int j = 1; j <= b_len; j++) {
            if (strcmp(a[i-1], b[j-1]) == 0) {
                dp[i][j] = dp[i-1][j-1] + 1;
            } else {
                dp[i][j] = dp[i-1][j] > dp[i][j-1] ? dp[i-1][j] : dp[i][j-1];
            }
        }
    }
    
    int lcs_max = dp[a_len][b_len];
    char **lcs = malloc(lcs_max * sizeof(char*));
    int lcs_count = 0;
    
    int i = a_len, j = b_len;
    while (i > 0 && j > 0) {
        if (strcmp(a[i-1], b[j-1]) == 0) {
            lcs[lcs_count++] = strdup(a[i-1]);
            i--; j--;
        } else if (dp[i-1][j] > dp[i][j-1]) {
            i--;
        } else {
            j--;
        }
    }
    
    for (int k = 0; k < lcs_count / 2; k++) {
        char *tmp = lcs[k];
        lcs[k] = lcs[lcs_count - 1 - k];
        lcs[lcs_count - 1 - k] = tmp;
    }
    
    for (i = 0; i <= a_len; i++) free(dp[i]);
    free(dp);
    
    *lcs_len = lcs_count;
    return lcs;
}

// Generate unified diff with specified context lines (max 10)
static char *generate_unified_diff(const char *filename, const char *old_content, const char *new_content, int context_lines) {
    Buffer buf;
    buffer_init(&buf);
    
    int old_count, new_count;
    char **old_lines = split_lines(old_content, &old_count);
    char **new_lines = split_lines(new_content, &new_count);
    
    if (!old_lines && !new_lines) {
        buffer_free(&buf);
        return strdup("");
    }
    
    int lcs_len;
    char **lcs = compute_lcs((const char**)old_lines, old_count, (const char**)new_lines, new_count, &lcs_len);
    
    char header[512];
    snprintf(header, sizeof(header), "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n", 
             filename, filename, filename, filename);
    buffer_append_str(&buf, header);
    
    int oi = 0, ni = 0, li = 0;
    
    while (oi < old_count || ni < new_count) {
        // Skip unchanged lines
        while (oi < old_count && ni < new_count && li < lcs_len &&
               strcmp(old_lines[oi], lcs[li]) == 0) {
            oi++; ni++; li++;
        }
        
        if (oi >= old_count && ni >= new_count) break;
        
        // Count context before changes
        int context_before = 0;
        int temp_oi = oi, temp_ni = ni, temp_li = li;
        while (context_before < context_lines && temp_oi > 0 && temp_ni > 0 && 
               temp_li > 0 && strcmp(old_lines[temp_oi-1], lcs[temp_li-1]) == 0) {
            context_before++;
            temp_oi--; temp_ni--; temp_li--;
        }
        
        int hunk_old_start = oi - context_before + 1;
        int hunk_new_start = ni - context_before + 1;
        
        Buffer hunk;
        buffer_init(&hunk);
        
        // Add context before
        for (int c = 0; c < context_before; c++) {
            char *line = old_lines[oi - context_before + c];
            buffer_append_str(&hunk, " ");
            buffer_append_str(&hunk, line);
            buffer_append(&hunk, "\n", 1);
        }
        
        int changes_started = 0;
        int trailing_context = 0;
        int hunk_old_count = 0, hunk_new_count = 0;
        
        while (oi < old_count || ni < new_count) {
            if (li < lcs_len && oi < old_count && ni < new_count &&
                strcmp(old_lines[oi], lcs[li]) == 0 && strcmp(new_lines[ni], lcs[li]) == 0) {
                if (changes_started) {
                    trailing_context++;
                    if (trailing_context > context_lines) break;
                }
                buffer_append_str(&hunk, " ");
                buffer_append_str(&hunk, old_lines[oi]);
                buffer_append(&hunk, "\n", 1);
                hunk_old_count++;
                hunk_new_count++;
                oi++; ni++; li++;
            } else if (oi < old_count && (li >= lcs_len || strcmp(old_lines[oi], lcs[li]) != 0)) {
                changes_started = 1;
                trailing_context = 0;
                buffer_append_str(&hunk, "-");
                buffer_append_str(&hunk, old_lines[oi]);
                buffer_append(&hunk, "\n", 1);
                hunk_old_count++;
                oi++;
            } else if (ni < new_count && (li >= lcs_len || strcmp(new_lines[ni], lcs[li]) != 0)) {
                changes_started = 1;
                trailing_context = 0;
                buffer_append_str(&hunk, "+");
                buffer_append_str(&hunk, new_lines[ni]);
                buffer_append(&hunk, "\n", 1);
                hunk_new_count++;
                ni++;
            } else {
                break;
            }
        }
        
        if (changes_started) {
            if (hunk_old_count == 0) hunk_old_count = 1;
            if (hunk_new_count == 0) hunk_new_count = 1;
            
            char hunk_header[128];
            snprintf(hunk_header, sizeof(hunk_header), "@@ -%d,%d +%d,%d @@\n",
                    hunk_old_start, hunk_old_count, hunk_new_start, hunk_new_count);
            buffer_append_str(&buf, hunk_header);
            buffer_append_str(&buf, buffer_c_str(&hunk));
        }
        buffer_free(&hunk);
    }
    
    if (lcs) {
        for (int i = 0; i < lcs_len; i++) free(lcs[i]);
        free(lcs);
    }
    if (old_lines) {
        for (int i = 0; i < old_count; i++) free(old_lines[i]);
        free(old_lines);
    }
    if (new_lines) {
        for (int i = 0; i < new_count; i++) free(new_lines[i]);
        free(new_lines);
    }
    
    char *result = strdup(buffer_c_str(&buf));
    buffer_free(&buf);
    return result;
}
