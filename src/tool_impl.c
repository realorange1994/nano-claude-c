#include "tool.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>

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

// Escape string for JSON
static char *json_escape(const char *str) {
    if (!str) return strdup("");
    
    Buffer buf;
    buffer_init(&buf);
    
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '\\': buffer_append_str(&buf, "\\\\"); break;
            case '"': buffer_append_str(&buf, "\\\""); break;
            case '\n': buffer_append_str(&buf, "\\n"); break;
            case '\r': buffer_append_str(&buf, "\\r"); break;
            case '\t': buffer_append_str(&buf, "\\t"); break;
            default: buffer_append(&buf, p, 1); break;
        }
    }
    
    char *result = buffer_c_str(&buf);
    char *ret = strdup(result);
    buffer_free(&buf);
    return ret;
}

// Built-in tool implementations
char *tool_read_file(cJSON *input, char **error) {
    cJSON *path = cJSON_GetObjectItem(input, "path");
    if (!path || !path->valuestring) {
        *error = strdup("missing path parameter");
        return NULL;
    }
    
    cJSON *offset = cJSON_GetObjectItem(input, "offset");
    cJSON *limit = cJSON_GetObjectItem(input, "limit");
    
    // Handle default values: offset defaults to 0, limit defaults to -1 (all)
    int off = 0;
    int lim = -1;
    
    if (offset && offset->type == cJSON_Number) {
        off = (int)offset->valuedouble;
    }
    if (limit && limit->type == cJSON_Number) {
        lim = (int)limit->valuedouble;
    }
    
    FILE *f = utf8_fopen(path->valuestring, "rb");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, off, SEEK_SET);
    
    if (lim < 0 || off + lim > size) {
        lim = size - off;
    }
    
    if (lim < 0) lim = 0;
    
    char *result = malloc(lim + 1);
    if (!result) {
        fclose(f);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    long read = fread(result, 1, lim, f);
    result[read] = '\0';
    fclose(f);
    
    return result;
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
    
    FILE *f = utf8_fopen(path->valuestring, "w");
    if (!f) {
        *error = strdup("failed to open file");
        return NULL;
    }
    
    fwrite(content->valuestring, 1, strlen(content->valuestring), f);
    fclose(f);
    
    return strdup("File written successfully");
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
    
    char *pos = strstr(content, old_str->valuestring);
    if (!pos) {
        free(content);
        *error = strdup("old_string not found in file");
        return NULL;
    }
    
    size_t old_len = strlen(old_str->valuestring);
    size_t new_len = strlen(new_str->valuestring);
    size_t diff = new_len - old_len;
    
    char *new_content = malloc(size + diff + 1);
    if (!new_content) {
        free(content);
        *error = strdup("memory allocation failed");
        return NULL;
    }
    
    memcpy(new_content, content, pos - content);
    memcpy(new_content + (pos - content), new_str->valuestring, new_len);
    memcpy(new_content + (pos - content) + new_len, pos + old_len, size - (pos - content) - old_len);
    new_content[size + diff] = '\0';
    
    free(content);
    
    f = utf8_fopen(path->valuestring, "w");
    if (!f) {
        free(new_content);
        *error = strdup("failed to open file for writing");
        return NULL;
    }
    
    fwrite(new_content, 1, strlen(new_content), f);
    fclose(f);
    free(new_content);
    
    return strdup("File edited successfully");
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
                char full_path[MAX_PATH];
                snprintf(full_path, sizeof(full_path), "%s\\%s", dir, FindFileData.cFileName);
                buffer_append_str(&buf, full_path);
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

    if (!cmd || !cmd->valuestring) {
        *error = strdup("missing command parameter");
        return NULL;
    }

    // Use git bash instead of cmd.exe for proper UTF-8 support
    const char *bash_paths[] = {
        "E:/Git/bin/bash.exe",
        "C:/Program Files/Git/bin/bash.exe",
        "C:/Program Files (x86)/Git/bin/bash.exe",
        NULL
    };

    // Escape single quotes in command for bash -c '...'
    int cmdlen = strlen(cmd->valuestring);
    char *escaped = malloc(cmdlen * 4 + 1);
    if (!escaped) {
        *error = strdup("out of memory");
        return NULL;
    }
    int j = 0;
    for (int i = 0; i < cmdlen; i++) {
        if (cmd->valuestring[i] == '\'') {
            escaped[j++] = '\'';
            escaped[j++] = '\\';
            escaped[j++] = '\'';
            escaped[j++] = '\'';
        } else {
            escaped[j++] = cmd->valuestring[i];
        }
    }
    escaped[j] = '\0';

    // Find bash executable
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

    // Build command line: "bash" -c 'command'
    char cmd_line[8192];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c '%s'", bash_exe, escaped);
    free(escaped);

    // Use CreateProcess with CREATE_NEW_PROCESS_GROUP so Ctrl+C is NOT sent to bash.
    // This is critical: _popen() inherits the console process group, so Ctrl+C kills bash.
    // With CREATE_NEW_PROCESS_GROUP, bash is in its own group and ignores Ctrl+C.
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead, hWrite;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    // Create pipe for stdout
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        *error = strdup("failed to create pipe");
        return NULL;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        *error = strdup("failed to create process");
        return NULL;
    }

    CloseHandle(hWrite);  // Close write end in parent

    // Read output
    Buffer buf;
    buffer_init(&buf);
    char chunk[4096];
    DWORD bytes_read;

    while (ReadFile(hRead, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        buffer_append_str(&buf, chunk);
    }

    CloseHandle(hRead);

    // Wait for process to finish
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Convert GBK output to UTF-8 for proper Chinese display
    char *result = buffer_c_str(&buf);
    char *utf8_result = convert_gbk_to_utf8(result);
    buffer_free(&buf);

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
