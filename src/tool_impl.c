#include "tool.h"
#include "rgrep.h"
#include "glob.h"
#include "calc.h"
#include "buffer.h"
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

static char *clean_utf8(const char *input);
static char *normalize_line_endings(const char *text);
static char *restore_line_endings(const char *text, int use_crlf);
static int fuzzy_find_text(const char *content, const char *search);

// LCS-based diff generation for EditDiff
static char **compute_lcs(const char **a, int a_len, const char **b, int b_len, int *lcs_len);
static char *generate_unified_diff(const char *filename, const char *old_content, const char *new_content, int context_lines);

// ============== Shell Security: Deny Keywords (simple substring matching) ==============
static const char *g_deny_keywords[] = {
    "rm -rf", "rm -fr", "rm -r ", "rm -r/",
    "rm -f ", "rm -f/",
    "rmdir /s", "rd /s", "del /s", "del /f", "del /q",
    "remove-item", "format c:", "format d:", "mkfs", "diskpart",
    "dd if=", "dd of=", "/dev/sd",
    "shutdown", "reboot", "poweroff",
    "docker system prune", "git push --force", "git push -f",
    "git clean -f", "git reset --hard",
    "git checkout --force", "git rebase --interactive", "git filter-branch",
    "git reflog expire",
    "fork bomb", "(){:", "(){ :",
    "&&&&&&",
    "eval", "base64 -d", "printf '\\\\x",
    NULL
};

// Check if command contains dangerous patterns using simple substring matching
static int check_deny_patterns(const char *cmd) {
    if (!cmd) return 0;

    char *lower = strdup(cmd);
    if (!lower) return 0;

    for (char *p = lower; *p; p++) {
        *p = tolower((unsigned char)*p);
    }

    for (size_t i = 0; g_deny_keywords[i]; i++) {
        if (strstr(lower, g_deny_keywords[i])) {
            free(lower);
            return 1;
        }
    }

    // Check for variable expansion bypass tricks: r$(echo m) -rf /
    if (strstr(lower, "$(") && strstr(lower, "rm")) {
        free(lower);
        return 1;
    }

    // Check for encoding bypasses: base64 -d | bash, printf | bash
    if ((strstr(lower, "base64") && strstr(lower, "|")) ||
        (strstr(lower, "printf") && strstr(lower, "|") && strstr(lower, "bash"))) {
        free(lower);
        return 1;
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

// Strip ANSI terminal escape codes (colors, cursor movement, etc.)
static char *strip_ansi_codes(const char *input) {
    if (!input) return NULL;
    
    // Pre-allocate with same size, worst case
    char *result = malloc(strlen(input) + 1);
    if (!result) return NULL;
    
    const char *src = input;
    char *dest = result;
    
    while (*src) {
        if (*src == '\x1B' || *src == '\033') {
            // Start of escape sequence
            if (src[1] == '[') {
                // CSI sequence
                src += 2;
                // Skip until we find a letter (end of sequence)
                while (*src && !((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z'))) {
                    src++;
                }
                if (*src) src++; // Skip the final letter
            } else if (src[1] == ']') {
                // OSC sequence (like \x1B]0;...\x07 or \x1B]0;...\x9C)
                src += 2;
                while (*src && *src != '\x07' && *src != '\x9C') {
                    src++;
                }
                if (*src) src++; // Skip \x07 or \x9C
            } else {
                // Other escape sequence
                src++;
            }
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
    
    return result;
}

// UTF-8 to wide string converter for file paths
static wchar_t *utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t *result = malloc(size * sizeof(wchar_t));
    if (!result) return NULL;  // Guard against malloc failure
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

// Escape string for JSON (reserved for future use)
// static char *json_escape(const char *str) { ... }

// Built-in tool implementations
// Max output size for Read tool (50KB)
#define MAX_READ_OUTPUT 51200
// Max lines to read
#define MAX_READ_LINES 1000
// Max line length
#define MAX_LINE_LENGTH 2000

// Helper: split text into lines (returns NULL-terminated array)
static char **split_text_lines(const char *text, int *count) {
    if (!text) { *count = 0; return NULL; }
    int cap = 128;
    char **lines = malloc(cap * sizeof(char*));
    if (!lines) return NULL;
    int n = 0;
    const char *start = text;
    while (*text && n < MAX_READ_LINES) {
        if (*text == '\n') {
            if (n >= cap - 1) {
                cap *= 2;
                char **tmp = realloc(lines, cap * sizeof(char*));
                if (!tmp) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
                lines = tmp;
            }
            size_t len = text - start;
            // Truncate very long lines
            if (len > MAX_LINE_LENGTH) len = MAX_LINE_LENGTH;
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
    if (*start && n < MAX_READ_LINES) {
        if (n >= cap - 1) {
            cap *= 2;
            char **tmp = realloc(lines, cap * sizeof(char*));
            if (!tmp) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
            lines = tmp;
        }
        size_t len = strlen(start);
        if (len > MAX_LINE_LENGTH) len = MAX_LINE_LENGTH;
        lines[n] = malloc(len + 1);
        if (!lines[n]) { for (int i = 0; i < n; i++) free(lines[i]); free(lines); return NULL; }
        memcpy(lines[n], start, len);
        lines[n][len] = '\0';
        n++;
    }
    *count = n;
    return lines;
}

// Wrapper for compatibility
static char **split_text_lines_safe(const char *text, int *count) {
    return split_text_lines(text, count);
}

char *tool_read_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }

    cJSON *offset = cJSON_GetObjectItem(input, "offset");
    cJSON *limit = cJSON_GetObjectItem(input, "limit");

    int line_offset = 0;
    int line_limit = MAX_READ_LINES;

    if (offset && offset->type == cJSON_Number) {
        int val = (int)offset->valuedouble;
        if (val < 1) {
            *error = strdup("offset must be >= 1");
            return NULL;
        }
        line_offset = val - 1;
    }
    if (limit && limit->type == cJSON_Number) {
        int val = (int)limit->valuedouble;
        if (val > 0 && val < MAX_READ_LINES) line_limit = val;
    }

    FILE *f = fopen(path->valuestring, "r");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }

    Buffer lines_buf;
    buffer_init(&lines_buf);

    char line[4096];
    int total_file_lines = 0;
    int lines_read = 0;

    while (fgets(line, sizeof(line), f)) {
        total_file_lines++;
        if (total_file_lines <= line_offset) continue;
        if (lines_read >= line_limit) continue;

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        buffer_append_str(&lines_buf, line);
        buffer_append_str(&lines_buf, "\n");
        lines_read++;
    }

    fclose(f);

    char *result = lines_buf.data ? lines_buf.data : strdup("");
    lines_buf.data = NULL;
    lines_buf.len = 0;
    lines_buf.capacity = 0;
    buffer_free(&lines_buf);

    Buffer out_buf;
    buffer_init(&out_buf);
    buffer_append_str(&out_buf, result);
    free(result);

    int more_lines = total_file_lines - (line_offset + lines_read);

    if (more_lines > 0) {
        char suffix[256];
        snprintf(suffix, sizeof(suffix), "\n\n[%d more lines in file. Use offset=%d to continue.]",
                 more_lines, line_offset + lines_read + 1);
        buffer_append_str(&out_buf, suffix);
    }

    char *output = out_buf.data ? out_buf.data : strdup("");
    out_buf.data = NULL;
    out_buf.len = 0;
    out_buf.capacity = 0;
    buffer_free(&out_buf);

    // Clean any invalid UTF-8 bytes to prevent  replacement characters
    char *dirty = output;
    output = clean_utf8(dirty);
    free(dirty);
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

// Replace invalid UTF-8 byte sequences with '?' to ensure valid output.
// This prevents Unicode replacement characters () in terminal output.
static char *clean_utf8(const char *input) {
    if (!input) return NULL;
    int len = strlen(input);
    char *output = malloc(len + 1);  // Worst case: same size
    if (!output) return strdup(input);
    int j = 0;
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)input[i];
        int expected_len = 0;
        if (c < 0x80) {
            expected_len = 1;  // ASCII
        } else if ((c & 0xE0) == 0xC0) {
            expected_len = 2;  // 110xxxxx
        } else if ((c & 0xF0) == 0xE0) {
            expected_len = 3;  // 1110xxxx
        } else if ((c & 0xF8) == 0xF0) {
            expected_len = 4;  // 11110xxx
        } else {
            // Invalid start byte (0x80-0xBF or 0xF8-0xFF)
            output[j++] = '?';
            i++;
            continue;
        }
        // Check if we have enough bytes remaining
        if (i + expected_len > (size_t)len) {
            output[j++] = '?';
            i++;
            continue;
        }
        // Validate continuation bytes
        int valid = 1;
        for (int k = 1; k < expected_len; k++) {
            if (((unsigned char)input[i + k] & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            output[j++] = '?';
            i++;
            continue;
        }
        // Check for overlong encoding
        if (expected_len == 2 && c < 0xC2) {
            // Overlong 2-byte sequence
            output[j++] = '?';
            i++;
            continue;
        }
        if (expected_len == 3 && c == 0xE0 && ((unsigned char)input[i + 1] & 0xE0) == 0x80) {
            // Overlong 3-byte sequence
            output[j++] = '?';
            i++;
            continue;
        }
        if (expected_len == 4 && c == 0xF0 && ((unsigned char)input[i + 1] & 0xF0) == 0x80) {
            // Overlong 4-byte sequence
            output[j++] = '?';
            i++;
            continue;
        }
        // Copy valid sequence
        for (int k = 0; k < expected_len; k++) {
            output[j++] = input[i + k];
        }
        i += expected_len;
    }
    output[j] = '\0';
    // If output is same size and no changes, free and return original copy
    if (j == len) {
        free(output);
        return strdup(input);
    }
    // Otherwise output has replacements
    return output;
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
    cJSON *pat = cJSON_GetObjectItem(input, "pattern");
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *glob = cJSON_GetObjectItem(input, "glob");
    cJSON *file_type = cJSON_GetObjectItem(input, "fileType");
    cJSON *context = cJSON_GetObjectItem(input, "context");
    cJSON *max_count = cJSON_GetObjectItem(input, "maxCount");
    cJSON *max_results = cJSON_GetObjectItem(input, "maxResults");
    cJSON *cs = cJSON_GetObjectItem(input, "caseSensitive");
    cJSON *inc_bin = cJSON_GetObjectItem(input, "includeBinary");
    cJSON *mll = cJSON_GetObjectItem(input, "maxLineLength");
    cJSON *omode = cJSON_GetObjectItem(input, "outputMode");

    if (!pat || !pat->valuestring) {
        *error = strdup("missing pattern parameter");
        return NULL;
    }

    RGrepConfig cfg = {0};
    cfg.pattern = pat->valuestring;
    cfg.path = (path && path->valuestring) ? path->valuestring : ".";
    cfg.glob = (glob && glob->valuestring) ? glob->valuestring : NULL;
    cfg.file_type = (file_type && file_type->valuestring) ? file_type->valuestring : NULL;
    cfg.context = (context && context->type == cJSON_Number) ? (int)context->valuedouble : 0;
    cfg.max_count = (max_count && max_count->type == cJSON_Number) ? (int)max_count->valuedouble : 100;
    cfg.max_results = (max_results && max_results->type == cJSON_Number) ? (int)max_results->valuedouble : 250;
    cfg.case_sensitive = cJSON_IsBool(cs) ? cs->valueint : 0;
    cfg.include_binary = cJSON_IsBool(inc_bin) ? inc_bin->valueint : 0;
    cfg.max_line_length = (mll && mll->type == cJSON_Number) ? (int)mll->valuedouble : 500;
    cfg.output_mode = OUTPUT_CONTENT;
    if (omode && omode->valuestring) {
        if (strcmp(omode->valuestring, "files_with_matches") == 0) cfg.output_mode = OUTPUT_FILES;
        else if (strcmp(omode->valuestring, "count") == 0) cfg.output_mode = OUTPUT_COUNT;
    }

    RGrepResult *result = rgrep_search(&cfg);
    if (!result) {
        *error = strdup("grep search failed");
        return NULL;
    }

    char *output = rgrep_get_output(result);
    char *final_output = output ? strdup(output) : strdup("");
    rgrep_free_result(result);
    return final_output;
}

char *tool_glob(cJSON *input, char **error) {
    cJSON *pat = cJSON_GetObjectItem(input, "pattern");
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *type = cJSON_GetObjectItem(input, "type");
    cJSON *max_results = cJSON_GetObjectItem(input, "maxResults");
    
    if (!pat || !pat->valuestring) {
        *error = strdup("missing pattern parameter");
        return NULL;
    }
    
    // Build config
    GlobConfig cfg = {0};
    cfg.pattern = pat->valuestring;
    cfg.path = (path && path->valuestring) ? path->valuestring : ".";
    cfg.max_results = (max_results && max_results->type == cJSON_Number) ? (int)max_results->valuedouble : 100;
    cfg.exclude_hidden = 1;  // Skip hidden files by default
    
    // Type filter
    if (type && type->valuestring) {
        if (strcmp(type->valuestring, "file") == 0) {
            cfg.type_filter = GLOB_TYPE_FILE;
        } else if (strcmp(type->valuestring, "dir") == 0) {
            cfg.type_filter = GLOB_TYPE_DIR;
        } else if (strcmp(type->valuestring, "all") == 0) {
            cfg.type_filter = GLOB_TYPE_ALL;
        } else {
            cfg.type_filter = GLOB_TYPE_FILE;  // Default to files
        }
    } else {
        cfg.type_filter = GLOB_TYPE_FILE;  // Default
    }
    
    // Perform glob search
    GlobResult *result = glob_search(&cfg);
    if (!result) {
        *error = strdup("glob search failed");
        return NULL;
    }
    
    char *output = glob_get_output(result);
    glob_free_result(result);
    
    return output ? output : strdup("");
}

char *tool_exec(cJSON *input, char **error) {
    cJSON *cmd = cJSON_GetObjectItem(input, "command");
    cJSON *cwd = cJSON_GetObjectItem(input, "cwd");
    cJSON *timeout = cJSON_GetObjectItem(input, "timeout");
    cJSON *env_obj = cJSON_GetObjectItem(input, "env");

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

    // Find bash executable
    const char *bash_paths[] = {
        "E:/Git/bin/bash.exe",
        "C:/Program Files/Git/bin/bash.exe",
        "C:/Program Files (x86)/Git/bin/bash.exe",
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
        *error = strdup("bash not found for command execution");
        return NULL;
    }

    // Build environment block
    char *env_block = NULL;
    if (env_obj && env_obj->type == cJSON_Object) {
        // Count existing env vars
        char **env_vars = NULL;
        int env_count = 0;
        
        // Get current environment
        char *env_str = GetEnvironmentStringsA();
        if (env_str) {
            char *p = env_str;
            while (*p) {
                env_count++;
                p += strlen(p) + 1;
            }
        }
        
        // Allocate array for env vars
        env_vars = calloc(env_count + cJSON_GetArraySize(env_obj) + 2, sizeof(char*));
        int idx = 0;
        
        // Copy existing environment
        if (env_str) {
            char *p = env_str;
            while (*p) {
                env_vars[idx++] = p;
                p += strlen(p) + 1;
            }
        }
        
        // Add/override with provided env vars
        cJSON *item;
        cJSON_ArrayForEach(item, env_obj) {
            const char *key = item->string;
            char *val = item->valuestring;
            if (key && val) {
                size_t len = strlen(key) + strlen(val) + 2;
                char *new_var = malloc(len);
                if (new_var) {
                    sprintf(new_var, "%s=%s", key, val);
                    env_vars[idx++] = new_var;
                }
            }
        }
        env_vars[idx++] = NULL;  // End marker
        
        // Convert to environment block (double-null terminated)
        size_t total_len = 0;
        for (int i = 0; env_vars[i]; i++) {
            total_len += strlen(env_vars[i]) + 1;
        }
        total_len += 1;  // Final null
        
        env_block = malloc(total_len);
        if (env_block) {
            char *dest = env_block;
            for (int i = 0; env_vars[i]; i++) {
                strcpy(dest, env_vars[i]);
                dest += strlen(env_vars[i]);
                *dest++ = '=';
            }
            *dest = '\0';
        }
        
        // Clean up temp allocations
        if (env_str) FreeEnvironmentStringsA(env_str);
        
        // Note: env_vars contains pointers to original strings, 
        // so we only free the new allocations
        cJSON *item2;
        cJSON_ArrayForEach(item2, env_obj) {
            // No-op, strings are in env_block
        }
        free(env_vars);
    }

    // Escape command for bash -c "..."
    // We use double quotes to wrap the command because CreateProcessW's CRT
    // argument parsing only understands double quotes (not single quotes).
    // Inner double quotes are escaped as \" for bash.
    int cmdlen = strlen(command);
    char *escaped = malloc(cmdlen * 4 + 1);
    if (!escaped) {
        free(env_block);
        *error = strdup("out of memory");
        return NULL;
    }
    int j = 0;
    for (int i = 0; i < cmdlen; i++) {
        if (command[i] == '"') {
            escaped[j++] = '\\';
            escaped[j++] = '"';
        } else {
            escaped[j++] = command[i];
        }
    }
    escaped[j] = '\0';

    // Get Windows temp dir for /tmp/ rewrite
    char win_temp[MAX_PATH];
    DWORD temp_len = GetTempPathA(sizeof(win_temp), win_temp);
    if (temp_len == 0 || temp_len >= sizeof(win_temp)) {
        strcpy(win_temp, "C:\\Windows\\Temp");
    }
    // Remove trailing backslash
    if (win_temp[strlen(win_temp) - 1] == '\\') {
        win_temp[strlen(win_temp) - 1] = '\0';
    }
    
    // Convert Windows temp to POSIX path
    char bash_temp[MAX_PATH];
    int ti = 0;
    for (int ci = 0; win_temp[ci] && ti < (int)(sizeof(bash_temp) - 1); ci++) {
        if (win_temp[ci] == '\\') {
            bash_temp[ti++] = '/';
        } else if (win_temp[ci] == ':') {
            // Skip drive letter colon, will handle in next char
        } else if (isalpha((unsigned char)win_temp[ci]) && ci == 0) {
            // Drive letter: C: -> /c
            bash_temp[ti++] = '/';
            bash_temp[ti++] = tolower((unsigned char)win_temp[ci]);
        } else {
            bash_temp[ti++] = tolower((unsigned char)win_temp[ci]);
        }
    }
    bash_temp[ti] = '\0';

    // Build command with /tmp/ rewrite
    char *final_cmd;
    if (strstr(escaped, "/tmp/") != NULL) {
        // Replace /tmp/ with bash_temp path
        size_t final_len = strlen(escaped) * 2 + strlen(bash_temp) + 10;
        final_cmd = malloc(final_len);
        if (final_cmd) {
            char *dest = final_cmd;
            const char *src = escaped;
            while (*src) {
                if (strncmp(src, "/tmp/", 5) == 0) {
                    strcpy(dest, bash_temp);
                    dest += strlen(bash_temp);
                    src += 5;
                } else {
                    *dest++ = *src++;
                }
            }
            *dest = '\0';
        } else {
            final_cmd = escaped;
        }
    } else {
        final_cmd = escaped;
    }

    // Build command line: "bash" -c "command" (double quotes for CreateProcessW)
    char cmd_line[16384];
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
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"cd %s && %s\"", bash_exe, posix_dir, final_cmd);
    } else {
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"%s\"", bash_exe, final_cmd);
    }
    
    if (final_cmd != escaped) free(final_cmd);
    free(escaped);

    // Create pipes for stdout and stderr
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;

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

    // Convert UTF-8 command line to wide chars for CreateProcessW.
    // CreateProcessA interprets the command line as ANSI (GBK on Chinese Windows),
    // which corrupts UTF-8 multi-byte sequences. CreateProcessW preserves Unicode.
    STARTUPINFOW siw;
    memset(&siw, 0, sizeof(siw));
    siw.cb = sizeof(siw);
    siw.dwFlags = STARTF_USESTDHANDLES;
    siw.hStdOutput = hWriteOut;
    siw.hStdError = hWriteErr;
    siw.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    wchar_t *w_cmd_line = utf8_to_wide(cmd_line);
    wchar_t *w_work_dir = work_dir ? utf8_to_wide(work_dir) : NULL;

    if (!CreateProcessW(NULL, w_cmd_line, NULL, NULL, TRUE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                        NULL, w_work_dir, &siw, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(hReadOut);
        CloseHandle(hWriteOut);
        CloseHandle(hReadErr);
        CloseHandle(hWriteErr);
        free(w_cmd_line);
        free(w_work_dir);
        free(env_block);
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "failed to create process (error %lu)", err);
        *error = strdup(errbuf);
        return NULL;
    }
    free(w_cmd_line);
    free(w_work_dir);
    free(env_block);

    CloseHandle(hWriteOut);  // Close write end in parent (stdout)
    CloseHandle(hWriteErr);  // Close write end in parent (stderr)

    // Read output with stall detection and Ctrl+C interruption
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
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
            Sleep(100);
            TerminateProcess(pi.hProcess, 1);
            buffer_append_str(&buf, "\n[Command timed out]\n");
            break;
        }

        // Check for Ctrl+C interruption (shared flag from console handler)
        if (g_interrupted) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
            Sleep(100);
            TerminateProcess(pi.hProcess, 1);
            buffer_append_str(&buf, "\n[Interrupted]\n");
            break;
        }

        // Use PeekNamedPipe to check for data without blocking
        // This allows us to check g_interrupted between reads
        if (!stdout_closed) {
            DWORD avail = 0;
            if (PeekNamedPipe(hReadOut, NULL, 0, NULL, &avail, NULL)) {
                if (avail > 0) {
                    if (ReadFile(hReadOut, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                        chunk[bytes_read] = '\0';
                        buffer_append_str(&buf, chunk);
                        last_activity = get_time_ms();
                    } else {
                        stdout_closed = 1;
                    }
                }
            } else {
                stdout_closed = 1;
            }
        }

        if (!stderr_closed) {
            DWORD avail = 0;
            if (PeekNamedPipe(hReadErr, NULL, 0, NULL, &avail, NULL)) {
                if (avail > 0) {
                    if (ReadFile(hReadErr, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                        chunk[bytes_read] = '\0';
                        buffer_append_str(&buf, chunk);
                        last_activity = get_time_ms();
                    } else {
                        stderr_closed = 1;
                    }
                }
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

        // Also check process exit if at least one pipe is closed
        if (running && (stdout_closed || stderr_closed)) {
            DWORD exitCode;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                // Process exited, read any remaining data then break
                Sleep(50);
                // Drain remaining stdout
                while (!stdout_closed) {
                    DWORD avail = 0;
                    if (PeekNamedPipe(hReadOut, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                        if (ReadFile(hReadOut, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                            chunk[bytes_read] = '\0';
                            buffer_append_str(&buf, chunk);
                        } else {
                            stdout_closed = 1;
                        }
                    } else {
                        stdout_closed = 1;
                    }
                }
                // Drain remaining stderr
                while (!stderr_closed) {
                    DWORD avail = 0;
                    if (PeekNamedPipe(hReadErr, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                        if (ReadFile(hReadErr, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
                            chunk[bytes_read] = '\0';
                            buffer_append_str(&buf, chunk);
                        } else {
                            stderr_closed = 1;
                        }
                    } else {
                        stderr_closed = 1;
                    }
                }
                break;
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
    // Get exit code BEFORE closing handles
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        exitCode = 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadOut);
    CloseHandle(hReadErr);

    char *utf8_result = buf.data ? buf.data : strdup("");
    buf.data = NULL;
    buf.len = 0;
    buf.capacity = 0;
    buffer_free(&buf);

    // Strip ANSI terminal codes
    char *clean_result = strip_ansi_codes(utf8_result);
    free(utf8_result);
    utf8_result = clean_result;

    // Append exit code if not 0
    char *final_result;
    size_t result_len = utf8_result ? strlen(utf8_result) : 0;
    if (exitCode != 0) {
        final_result = malloc(result_len + 64);
        if (final_result) {
            snprintf(final_result, result_len + 64, "%s\nExit code: %lu", utf8_result, (unsigned long)exitCode);
            free(utf8_result);
            return final_result;
        }
    }

    return utf8_result ? utf8_result : strdup("");
}

char *tool_calc(cJSON *input, char **error) {
    cJSON *expr = cJSON_GetObjectItem(input, "expr");
    
    if (!expr || !expr->valuestring) {
        *error = strdup("missing expr parameter");
        return NULL;
    }
    
    double result = calc_evaluate(expr->valuestring, error);
    
    if (isnan(result)) return strdup("NaN");
    if (isinf(result)) return strdup(result > 0 ? "Infinity" : "-Infinity");
    
    // Format: integers without decimal, others with precision
    if (result == (double)(int64_t)result) {
        char *str = malloc(32);
        if (str) snprintf(str, 32, "%.0f", result);
        return str;
    }
    return calc_format_result(result);
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

    // Clean any invalid UTF-8 bytes to prevent  replacement characters
    char *dirty = strdup(buffer_c_str(&buf));
    buffer_free(&buf);
    char *result = clean_utf8(dirty);
    free(dirty);
    return result;
}
