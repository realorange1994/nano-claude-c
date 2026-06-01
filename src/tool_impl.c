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

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#endif

// ============== Millisecond timer ==============
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ============== Recursive mkdir ==============
static void mkdirs(const char *path) {
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

// ============== UTF-8 to wide string (Windows only) ==============
#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t *result = malloc(size * sizeof(wchar_t));
    if (!result) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result, size);
    return result;
}

static FILE *utf8_fopen(const char *path, const char *mode) {
    wchar_t *w_path = utf8_to_wide(path);
    wchar_t *w_mode = utf8_to_wide(mode);
    if (!w_path || !w_mode) {
        free(w_path); free(w_mode);
        return fopen(path, mode);
    }
    FILE *f = _wfopen(w_path, w_mode);
    free(w_path); free(w_mode);
    return f;
}
#else
#define utf8_fopen fopen
#endif

// ============== Shell Security ==============
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

static int check_deny_patterns(const char *cmd) {
    if (!cmd) return 0;
    char *lower = strdup(cmd);
    if (!lower) return 0;
    for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);
    for (size_t i = 0; g_deny_keywords[i]; i++) {
        if (strstr(lower, g_deny_keywords[i])) { free(lower); return 1; }
    }
    if (strstr(lower, "$(") && strstr(lower, "rm")) { free(lower); return 1; }
    if ((strstr(lower, "base64") && strstr(lower, "|")) ||
        (strstr(lower, "printf") && strstr(lower, "|") && strstr(lower, "bash"))) {
        free(lower); return 1;
    }
    free(lower);
    return 0;
}

// ============== Interactive prompt detection ==============
static const char *g_interactive_patterns[] = {
    "(y/n)", "[y/n]", "(yes/no)", "press any key", "press enter",
    "continue?", "overwrite?", "password:", "passphrase:",
    "enter password", "sudo: password", ">>> $",
    NULL
};

static const char *match_interactive_prompt(const char *text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    if (len > 1024) text = text + len - 1024;
    for (size_t i = 0; g_interactive_patterns[i]; i++) {
        if (strstr(text, g_interactive_patterns[i])) return g_interactive_patterns[i];
    }
    return NULL;
}

// ============== Strip ANSI codes ==============
static char *strip_ansi_codes(const char *input) {
    if (!input) return NULL;
    char *result = malloc(strlen(input) + 1);
    if (!result) return NULL;
    const char *src = input;
    char *dest = result;
    while (*src) {
        if (*src == '\x1B' || *src == '\033') {
            if (src[1] == '[') {
                src += 2;
                while (*src && !((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z'))) src++;
                if (*src) src++;
            } else if (src[1] == ']') {
                src += 2;
                while (*src && *src != '\x07' && *src != '\x9C') src++;
                if (*src) src++;
            } else {
                src++;
            }
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
    return result;
}

// ============== Clean UTF-8 ==============
static char *clean_utf8(const char *input) {
    if (!input) return NULL;
    int len = strlen(input);
    char *output = malloc(len + 1);
    if (!output) return strdup(input);
    int j = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)input[i];
        int expected_len = 0;
        if (c < 0x80) expected_len = 1;
        else if ((c & 0xE0) == 0xC0) expected_len = 2;
        else if ((c & 0xF0) == 0xE0) expected_len = 3;
        else if ((c & 0xF8) == 0xF0) expected_len = 4;
        else { output[j++] = '?'; i++; continue; }
        if (i + expected_len > len) { output[j++] = '?'; i++; continue; }
        int valid = 1;
        for (int k = 1; k < expected_len; k++) {
            if (((unsigned char)input[i + k] & 0xC0) != 0x80) { valid = 0; break; }
        }
        if (!valid || (expected_len == 2 && c < 0xC2)) { output[j++] = '?'; i++; continue; }
        for (int k = 0; k < expected_len; k++) output[j++] = input[i + k];
        i += expected_len;
    }
    output[j] = '\0';
    if (j == len) { free(output); return strdup(input); }
    return output;
}

// ============== Read tool ==============
#define MAX_READ_OUTPUT 51200
#define MAX_READ_LINES 1000

char *tool_read_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    if (!path || !path->valuestring) { *error = strdup("missing path"); return NULL; }

    cJSON *offset = cJSON_GetObjectItem(input, "offset");
    cJSON *limit = cJSON_GetObjectItem(input, "limit");

    int line_offset = 0;
    int line_limit = MAX_READ_LINES;
    if (offset && offset->type == cJSON_Number) {
        int val = (int)offset->valuedouble;
        if (val < 1) { *error = strdup("offset must be >= 1"); return NULL; }
        line_offset = val - 1;
    }
    if (limit && limit->type == cJSON_Number) {
        int val = (int)limit->valuedouble;
        if (val > 0 && val < MAX_READ_LINES) line_limit = val;
    }

    FILE *f = utf8_fopen(path->valuestring, "rb");
    if (!f) { *error = strdup("failed to open file"); return NULL; }

    // Read entire file content in binary mode
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *raw_content = malloc(fsize + 1);
    if (!raw_content) { fclose(f); *error = strdup("out of memory"); return NULL; }
    size_t read_bytes = fread(raw_content, 1, fsize, f);
    raw_content[read_bytes] = '\0';
    fclose(f);

    // Strip BOM if present
    char *content_start = raw_content;
    if (read_bytes >= 3 && (unsigned char)raw_content[0] == 0xEF &&
        (unsigned char)raw_content[1] == 0xBB && (unsigned char)raw_content[2] == 0xBF) {
        content_start = raw_content + 3;
    }

    // Process line by line from the UTF-8 content
    Buffer lines_buf;
    buffer_init(&lines_buf);
    char line[4096];
    int total_file_lines = 0;
    int lines_read = 0;

    // Split content into lines
    char *ptr = content_start;
    while (*ptr) {
        char *line_end = strchr(ptr, '\n');
        if (line_end) {
            *line_end = '\0';
            // Also strip \r
            size_t line_len = strlen(ptr);
            if (line_len > 0 && ptr[line_len - 1] == '\r') ptr[--line_len] = '\0';

            total_file_lines++;
            if (total_file_lines > line_offset && lines_read < line_limit) {
                buffer_append_str(&lines_buf, ptr);
                buffer_append_str(&lines_buf, "\n");
                lines_read++;
            }

            ptr = line_end + 1;
        } else {
            // Last line (no trailing newline)
            if (*ptr) {
                total_file_lines++;
                if (total_file_lines > line_offset && lines_read < line_limit) {
                    buffer_append_str(&lines_buf, ptr);
                    buffer_append_str(&lines_buf, "\n");
                    lines_read++;
                }
            }
            break;
        }
    }
    free(raw_content);

    char *result = lines_buf.data ? lines_buf.data : strdup("");
    lines_buf.data = NULL; lines_buf.len = 0; lines_buf.capacity = 0;
    buffer_free(&lines_buf);

    Buffer out_buf;
    buffer_init(&out_buf);
    buffer_append_str(&out_buf, result);
    free(result);

    int more_lines = total_file_lines - (line_offset + lines_read);
    if (more_lines > 0) {
        char suffix[256];
        snprintf(suffix, sizeof(suffix), "\n\n[%d more lines. Use offset=%d]", more_lines, line_offset + lines_read + 1);
        buffer_append_str(&out_buf, suffix);
    }

    char *output = out_buf.data ? out_buf.data : strdup("");
    out_buf.data = NULL; out_buf.len = 0; out_buf.capacity = 0;
    buffer_free(&out_buf);

    char *dirty = output;
    output = clean_utf8(dirty);
    free(dirty);
    return output;
}

// ============== Write tool ==============
char *tool_write_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    if (!path || !path->valuestring) { *error = strdup("missing path"); return NULL; }
    if (!content || !content->valuestring) { *error = strdup("missing content"); return NULL; }

    // Create parent directories
    char dir_buf[4096];
    strncpy(dir_buf, path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *last_sep = strrchr(dir_buf, '/');
    if (!last_sep) last_sep = strrchr(dir_buf, '\\');
    if (last_sep) {
        *last_sep = '\0';
        mkdirs(dir_buf);
    }

    FILE *f = utf8_fopen(path, "wb");
    if (!f) { *error = strdup("failed to open file for writing"); return NULL; }

    fwrite(content->valuestring, 1, strlen(content->valuestring), f);
    fclose(f);

    size_t content_len = strlen(content->valuestring);
    char *result = malloc(64 + strlen(path->valuestring) + 20);
    if (result) sprintf(result, "Successfully wrote %zu bytes to %s", content_len, path->valuestring);
    else result = strdup("File written successfully");
    return result;
}

// ============== Edit tool ==============
static char *normalize_line_endings(const char *text) {
    if (!text) return NULL;
    int len = strlen(text);
    char *result = malloc(len + 1);
    if (!result) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\r') {
            if (i + 1 < len && text[i + 1] == '\n') { result[j++] = '\n'; i++; }
            else result[j++] = '\n';
        } else {
            result[j++] = text[i];
        }
    }
    result[j] = '\0';
    return result;
}

static char *restore_line_endings(const char *text, int use_crlf) {
    if (!text) return NULL;
    int len = strlen(text);
    int count_lf = 0;
    for (int i = 0; i < len; i++) if (text[i] == '\n') count_lf++;
    int new_len = len + (use_crlf ? count_lf : 0);
    char *result = malloc(new_len + 1);
    if (!result) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n' && use_crlf) result[j++] = '\r';
        result[j++] = text[i];
    }
    result[j] = '\0';
    return result;
}

static int fuzzy_find_text(const char *content, const char *search) {
    if (!content || !search) return -1;
    // Case-insensitive search
    size_t plen = strlen(search);
    size_t clen = strlen(content);
    if (plen > clen) return -1;
    for (size_t i = 0; i <= clen - plen; i++) {
        int match = 1;
        for (size_t j = 0; j < plen; j++) {
            if (tolower((unsigned char)content[i+j]) != tolower((unsigned char)search[j])) { match = 0; break; }
        }
        if (match) return (int)i;
    }
    return -1;
}

char *tool_edit_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    cJSON *old_str = cJSON_GetObjectItem(input, "old_string");
    cJSON *new_str = cJSON_GetObjectItem(input, "new_string");
    if (!path || !path->valuestring) { *error = strdup("missing path"); return NULL; }
    if (!old_str || !old_str->valuestring) { *error = strdup("missing old_string"); return NULL; }
    if (!new_str || !new_str->valuestring) { *error = strdup("missing new_string"); return NULL; }

    FILE *f = utf8_fopen(path->valuestring, "rb");
    if (!f) { *error = strdup("failed to open file"); return NULL; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(size + 1);
    if (!content) { fclose(f); *error = strdup("out of memory"); return NULL; }
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);

    // Strip BOM
    char *file_content = content;
    int bom_len = 0;
    if (size >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        bom_len = 3; file_content = content + 3;
    }

    // Detect line ending
    int has_crlf = 0;
    for (int i = 0; i < size - bom_len; i++) {
        if (content[i] == '\r' && i + 1 < size && content[i + 1] == '\n') { has_crlf = 1; break; }
    }

    char *normalized = normalize_line_endings(file_content);
    char *normalized_old = normalize_line_endings(old_str->valuestring);
    char *normalized_new = normalize_line_endings(new_str->valuestring);
    if (!normalized || !normalized_old || !normalized_new) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        *error = strdup("out of memory"); return NULL;
    }

    char *pos = strstr(normalized, normalized_old);
    int match_len = 0;
    if (!pos) {
        int fuzzy_idx = fuzzy_find_text(normalized, normalized_old);
        if (fuzzy_idx >= 0) pos = normalized + fuzzy_idx;
    }
    if (pos) match_len = strlen(normalized_old);
    if (!pos) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        *error = strdup("old_string not found in file"); return NULL;
    }

    // Check uniqueness
    int occurrences = 0;
    char *check_pos = normalized;
    while ((check_pos = strstr(check_pos, normalized_old)) != NULL) { occurrences++; check_pos++; }
    if (occurrences > 1) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "Found %d occurrences. Text must be unique.", occurrences);
        *error = strdup(err_buf); return NULL;
    }

    int match_offset = pos - normalized;
    int new_size = match_offset + strlen(normalized_new) + (strlen(normalized) - match_offset - match_len);
    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        *error = strdup("out of memory"); return NULL;
    }

    memcpy(new_content, normalized, match_offset);
    memcpy(new_content + match_offset, normalized_new, strlen(normalized_new));
    memcpy(new_content + match_offset + strlen(normalized_new), normalized + match_offset + match_len, strlen(normalized) - match_offset - match_len);
    new_content[new_size] = '\0';

    if (has_crlf) {
        char *restored = restore_line_endings(new_content, 1);
        free(new_content);
        if (!restored) { free(content); free(normalized); free(normalized_old); free(normalized_new); *error = strdup("out of memory"); return NULL; }
        new_content = restored;
    }

    if (strcmp(file_content, new_content) == 0) {
        free(content); free(normalized); free(normalized_old); free(normalized_new); free(new_content);
        *error = strdup("No changes made"); return NULL;
    }

    f = utf8_fopen(path->valuestring, "wb");
    if (!f) { free(content); free(normalized); free(normalized_old); free(normalized_new); free(new_content); *error = strdup("failed to write"); return NULL; }
    if (bom_len > 0) fwrite("\xEF\xBB\xBF", 1, 3, f);
    fwrite(new_content, 1, strlen(new_content), f);
    fclose(f);

    free(content); free(normalized); free(normalized_old); free(normalized_new); free(new_content);
    return strdup("File edited successfully");
}

// ============== Grep tool ==============
char *tool_grep(cJSON *input, char **error) {
    cJSON *pat = cJSON_GetObjectItem(input, "pattern");
    if (!pat || !pat->valuestring) { *error = strdup("missing pattern"); return NULL; }

    RGrepConfig cfg = {0};
    cfg.pattern = pat->valuestring;

    cJSON *item;
    item = cJSON_GetObjectItem(input, "path"); cfg.path = (item && item->valuestring) ? item->valuestring : ".";
    item = cJSON_GetObjectItem(input, "glob"); cfg.glob = (item && item->valuestring) ? item->valuestring : NULL;
    item = cJSON_GetObjectItem(input, "fileType"); cfg.file_type = (item && item->valuestring) ? item->valuestring : NULL;
    item = cJSON_GetObjectItem(input, "context"); cfg.context = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 0;
    item = cJSON_GetObjectItem(input, "maxCount"); cfg.max_count = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 100;
    item = cJSON_GetObjectItem(input, "maxResults"); cfg.max_results = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 250;
    cfg.case_sensitive = 0; cfg.include_binary = 0; cfg.max_line_length = 500; cfg.output_mode = OUTPUT_CONTENT;

    item = cJSON_GetObjectItem(input, "outputMode");
    if (item && item->valuestring) {
        if (strcmp(item->valuestring, "files_with_matches") == 0) cfg.output_mode = OUTPUT_FILES;
        else if (strcmp(item->valuestring, "count") == 0) cfg.output_mode = OUTPUT_COUNT;
    }

    RGrepResult *result = rgrep_search(&cfg);
    if (!result) { *error = strdup("grep failed"); return NULL; }
    char *output = rgrep_get_output(result);
    char *final = output ? strdup(output) : strdup("");
    rgrep_free_result(result);
    return final;
}

// ============== Glob tool ==============
char *tool_glob(cJSON *input, char **error) {
    cJSON *pat = cJSON_GetObjectItem(input, "pattern");
    if (!pat || !pat->valuestring) { *error = strdup("missing pattern"); return NULL; }

    GlobConfig cfg = {0};
    cfg.pattern = pat->valuestring;
    cJSON *p_item;
    p_item = cJSON_GetObjectItem(input, "path"); cfg.path = (p_item && p_item->valuestring) ? p_item->valuestring : ".";
    p_item = cJSON_GetObjectItem(input, "maxResults"); cfg.max_results = (p_item && p_item->type == cJSON_Number) ? (int)p_item->valuedouble : 100;
    cfg.exclude_hidden = 1; cfg.type_filter = GLOB_TYPE_FILE;

    p_item = cJSON_GetObjectItem(input, "type");
    if (p_item && p_item->valuestring) {
        if (strcmp(p_item->valuestring, "dir") == 0) cfg.type_filter = GLOB_TYPE_DIR;
        else if (strcmp(p_item->valuestring, "all") == 0) cfg.type_filter = GLOB_TYPE_ALL;
    }

    GlobResult *result = glob_search(&cfg);
    if (!result) { *error = strdup("glob failed"); return NULL; }
    char *output = glob_get_output(result);
    glob_free_result(result);
    return output ? output : strdup("");
}

// ============== Shell tool ==============
char *tool_exec(cJSON *input, char **error) {
    cJSON *cmd = cJSON_GetObjectItem(input, "command");
    cJSON *cwd = cJSON_GetObjectItem(input, "cwd");
    cJSON *timeout = cJSON_GetObjectItem(input, "timeout");

    if (!cmd || !cmd->valuestring) { *error = strdup("missing command"); return NULL; }

    const char *command = cmd->valuestring;
    const char *work_dir = cwd && cwd->valuestring ? cwd->valuestring : NULL;
    int timeout_sec = 120;
    if (timeout && timeout->type == cJSON_Number) {
        timeout_sec = (int)timeout->valuedouble;
        if (timeout_sec <= 0) timeout_sec = 120;
        if (timeout_sec > 600) timeout_sec = 600;
    }

    if (check_deny_patterns(command)) { *error = strdup("dangerous command detected"); return NULL; }

#ifdef _WIN32
    // Windows: CreateProcess with pipes
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hReadOut, &hWriteOut, &sa, 0)) { *error = strdup("pipe failed"); return NULL; }
    SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&hReadErr, &hWriteErr, &sa, 0)) { CloseHandle(hReadOut); CloseHandle(hWriteOut); *error = strdup("pipe failed"); return NULL; }
    SetHandleInformation(hReadErr, HANDLE_FLAG_INHERIT, 0);

    // Find bash
    const char *bash_paths[] = { "E:/Git/bin/bash.exe", "C:/Program Files/Git/bin/bash.exe", NULL };
    const char *bash_exe = NULL;
    for (int i = 0; bash_paths[i]; i++) {
        if (GetFileAttributesA(bash_paths[i]) != INVALID_FILE_ATTRIBUTES) { bash_exe = bash_paths[i]; break; }
    }
    if (!bash_exe) { *error = strdup("bash not found"); CloseHandle(hReadOut); CloseHandle(hWriteOut); CloseHandle(hReadErr); CloseHandle(hWriteErr); return NULL; }

    char cmd_line[16384];
    if (work_dir) {
        char posix_dir[512]; int pi = 0;
        for (int ci = 0; work_dir[ci] && pi < (int)(sizeof(posix_dir)-1); ci++) {
            if (work_dir[ci] == '\\') posix_dir[pi++] = '/';
            else if (work_dir[ci] == ':') { posix_dir[pi++] = '/'; posix_dir[pi++] = tolower(work_dir[++ci]); }
            else posix_dir[pi++] = tolower(work_dir[ci]);
        }
        posix_dir[pi] = '\0';
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"cd %s && %s\"", bash_exe, posix_dir, command);
    } else {
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"%s\"", bash_exe, command);
    }

    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW siw = {0};
    siw.cb = sizeof(siw);
    siw.dwFlags = STARTF_USESTDHANDLES;
    siw.hStdOutput = hWriteOut;
    siw.hStdError = hWriteErr;
    siw.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    wchar_t *w_cmd = utf8_to_wide(cmd_line);
    wchar_t *w_cwd = work_dir ? utf8_to_wide(work_dir) : NULL;

    if (!CreateProcessW(NULL, w_cmd, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, NULL, w_cwd, &siw, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(hReadOut); CloseHandle(hWriteOut); CloseHandle(hReadErr); CloseHandle(hWriteErr);
        free(w_cmd); free(w_cwd);
        char errbuf[128]; snprintf(errbuf, sizeof(errbuf), "CreateProcess failed (%lu)", err);
        *error = strdup(errbuf); return NULL;
    }
    free(w_cmd); free(w_cwd);
    CloseHandle(hWriteOut); CloseHandle(hWriteErr);

    Buffer buf; buffer_init(&buf);
    char chunk[4096]; DWORD bytes_read;
    long long start_time = get_time_ms();
    long long last_activity = start_time;
    int stall_threshold_ms = (timeout_sec * 1000) / 3;
    if (stall_threshold_ms < 10000) stall_threshold_ms = 10000;

    while (1) {
        long long now = get_time_ms();
        if (now - start_time > timeout_sec * 1000) {
            TerminateProcess(pi.hProcess, 1);
            buffer_append_str(&buf, "\n[Command timed out]\n"); break;
        }
        if (g_interrupted) {
            TerminateProcess(pi.hProcess, 1);
            buffer_append_str(&buf, "\n[Interrupted]\n"); break;
        }

        DWORD avail = 0;
        int got_data = 0;
        if (PeekNamedPipe(hReadOut, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (ReadFile(hReadOut, chunk, sizeof(chunk)-1, &bytes_read, NULL) && bytes_read > 0) {
                chunk[bytes_read] = '\0'; buffer_append_str(&buf, chunk); last_activity = now; got_data = 1;
            }
        }
        if (PeekNamedPipe(hReadErr, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (ReadFile(hReadErr, chunk, sizeof(chunk)-1, &bytes_read, NULL) && bytes_read > 0) {
                chunk[bytes_read] = '\0'; buffer_append_str(&buf, chunk); last_activity = now; got_data = 1;
            }
        }

        // Check process exit
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            // Drain remaining
            Sleep(50);
            while (PeekNamedPipe(hReadOut, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (ReadFile(hReadOut, chunk, sizeof(chunk)-1, &bytes_read, NULL) && bytes_read > 0) { chunk[bytes_read] = '\0'; buffer_append_str(&buf, chunk); }
            }
            while (PeekNamedPipe(hReadErr, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (ReadFile(hReadErr, chunk, sizeof(chunk)-1, &bytes_read, NULL) && bytes_read > 0) { chunk[bytes_read] = '\0'; buffer_append_str(&buf, chunk); }
            }
            break;
        }

        // Stall detection
        now = get_time_ms();
        if (!got_data && (now - last_activity) > stall_threshold_ms) {
            const char *tail = buffer_c_str(&buf);
            if (tail && match_interactive_prompt(tail)) {
                TerminateProcess(pi.hProcess, 1);
                buffer_append_str(&buf, "\n[Command may be waiting for input]\n"); break;
            }
            last_activity = now;
        }

        Sleep(10);
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(hReadOut); CloseHandle(hReadErr);

    char *utf8_result = buf.data ? buf.data : strdup("");
    buf.data = NULL; buffer_free(&buf);
    char *clean_result = strip_ansi_codes(utf8_result);
    free(utf8_result);

    if (exitCode != 0) {
        size_t rlen = strlen(clean_result);
        char *final = malloc(rlen + 64);
        if (final) { snprintf(final, rlen + 64, "%s\nExit code: %lu", clean_result, (unsigned long)exitCode); free(clean_result); return final; }
    }
    return clean_result;

#else
    // Linux: fork/exec with pipes
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) { *error = strdup("pipe failed"); return NULL; }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        *error = strdup("fork failed"); return NULL;
    }

    if (pid == 0) {
        close(stdout_pipe[0]); close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]); close(stderr_pipe[1]);
        if (work_dir) chdir(work_dir);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(stdout_pipe[1]); close(stderr_pipe[1]);

    Buffer buf; buffer_init(&buf);
    char chunk[4096];
    long long start_time = get_time_ms();
    long long last_activity = start_time;
    int stall_threshold_ms = (timeout_sec * 1000) / 3;
    if (stall_threshold_ms < 10000) stall_threshold_ms = 10000;

    int status = 0;  // Declared outside loop so we can use it after

    while (1) {
        long long now = get_time_ms();
        if (now - start_time > timeout_sec * 1000) {
            kill(pid, SIGKILL); buffer_append_str(&buf, "\n[Command timed out]\n"); break;
        }
        if (g_interrupted) {
            kill(pid, SIGKILL); buffer_append_str(&buf, "\n[Interrupted]\n"); break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(stdout_pipe[0], &readfds);
        FD_SET(stderr_pipe[0], &readfds);
        int maxfd = (stdout_pipe[0] > stderr_pipe[0]) ? stdout_pipe[0] : stderr_pipe[0];
        struct timeval tv = {0, 100000}; // 100ms
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        int got_data = 0;
        if (ready > 0) {
            if (FD_ISSET(stdout_pipe[0], &readfds)) {
                ssize_t n = read(stdout_pipe[0], chunk, sizeof(chunk)-1);
                if (n > 0) { chunk[n] = '\0'; buffer_append_str(&buf, chunk); last_activity = now; got_data = 1; }
            }
            if (FD_ISSET(stderr_pipe[0], &readfds)) {
                ssize_t n = read(stderr_pipe[0], chunk, sizeof(chunk)-1);
                if (n > 0) { chunk[n] = '\0'; buffer_append_str(&buf, chunk); last_activity = now; got_data = 1; }
            }
        }

        // Check if child exited
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result > 0) {
            // Drain remaining output
            while (1) {
                ssize_t n = read(stdout_pipe[0], chunk, sizeof(chunk)-1);
                if (n <= 0) break;
                chunk[n] = '\0'; buffer_append_str(&buf, chunk);
            }
            while (1) {
                ssize_t n = read(stderr_pipe[0], chunk, sizeof(chunk)-1);
                if (n <= 0) break;
                chunk[n] = '\0'; buffer_append_str(&buf, chunk);
            }
            break;
        }

        // Stall detection
        now = get_time_ms();
        if (!got_data && (now - last_activity) > stall_threshold_ms) {
            const char *tail = buffer_c_str(&buf);
            if (tail && match_interactive_prompt(tail)) {
                kill(pid, SIGKILL);
                buffer_append_str(&buf, "\n[Command may be waiting for input]\n"); break;
            }
            last_activity = now;
        }
    }

    close(stdout_pipe[0]); close(stderr_pipe[0]);

    // If the loop broke without reaping the child (timeout/interrupt), waitpid to reap it
    if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
        waitpid(pid, &status, 0);
    }

    char *utf8_result = buf.data ? buf.data : strdup("");
    buf.data = NULL; buffer_free(&buf);
    char *clean_result = strip_ansi_codes(utf8_result);
    free(utf8_result);

    if (WIFEXITED(status)) {
        size_t rlen = strlen(clean_result);
        char *final = malloc(rlen + 64);
        if (final) { snprintf(final, rlen + 64, "%s\nExit code: %d", clean_result, WEXITSTATUS(status)); free(clean_result); return final; }
    } else if (WIFSIGNALED(status)) {
        size_t rlen = strlen(clean_result);
        char *final = malloc(rlen + 64);
        if (final) { snprintf(final, rlen + 64, "%s\nKilled by signal: %d", clean_result, WTERMSIG(status)); free(clean_result); return final; }
    }

    return clean_result;
#endif
}

// ============== Calc tool ==============
char *tool_calc(cJSON *input, char **error) {
    cJSON *expr = cJSON_GetObjectItem(input, "expr");
    if (!expr || !expr->valuestring) { *error = strdup("missing expr"); return NULL; }

    double result = calc_evaluate(expr->valuestring, error);
    if (isnan(result)) return strdup("NaN");
    if (isinf(result)) return strdup(result > 0 ? "Infinity" : "-Infinity");
    if (result == (double)(int64_t)result) {
        char *str = malloc(32);
        if (str) snprintf(str, 32, "%.0f", result);
        return str;
    }
    return calc_format_result(result);
}
