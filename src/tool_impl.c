#include "tool.h"
#include "rgrep.h"
#include "glob.h"
#include "calc.h"
#include "buffer.h"
#include "history.h"
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
#include <sys/stat.h>
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode) & _S_IFDIR)
#endif
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

// ============== UTF-8 file operations (Windows only) ==============
#ifdef _WIN32
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

// ============== Path expansion ==============
static char *expand_path(const char *path) {
    if (!path) return NULL;
    // Expand ~ to home directory
    if (path[0] == '~') {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
#else
        const char *home = getenv("HOME");
#endif
        if (home) {
            size_t hlen = strlen(home);
            size_t plen = strlen(path);
            char *expanded = malloc(hlen + plen);
            if (!expanded) return strdup(path);
            memcpy(expanded, home, hlen);
            memcpy(expanded + hlen, path + 1, plen); // skip ~, include \0
            return expanded;
        }
    }
    return strdup(path);
}

// ============== Binary extension check ==============
static int is_binary_extension(const char *ext) {
    if (!ext) return 0;
    // Lowercase compare
    char lower[16];
    size_t len = strlen(ext);
    if (len >= sizeof(lower)) return 0;
    for (size_t i = 0; i <= len; i++) lower[i] = tolower((unsigned char)ext[i]);

    static const char *binary_exts[] = {
        ".exe", ".dll", ".so", ".dylib", ".com",
        ".zip", ".tar", ".gz", ".bz2", ".xz", ".7z", ".rar", ".tgz", ".zst",
        ".cab", ".iso", ".img", ".dmg",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tiff", ".ico", ".webp",
        ".mp3", ".mp4", ".wav", ".ogg", ".avi", ".mov", ".mkv", ".flac",
        ".pyc", ".pyo", ".o", ".obj", ".a", ".lib", ".class", ".jar",
        ".dat", ".bin", ".db", ".sqlite", ".pdf", ".docx", ".xlsx", ".pptx",
        ".woff", ".woff2", ".eot", ".ttf",
        NULL
    };
    for (int i = 0; binary_exts[i]; i++) {
        if (strcmp(lower, binary_exts[i]) == 0) return 1;
    }
    return 0;
}

// ============== Binary magic bytes check ==============
static int is_binary_magic(const unsigned char *header, int len) {
    if (len < 2) return 0;
    // PE/EXE: MZ
    if (header[0] == 'M' && header[1] == 'Z') return 1;
    // GZIP: 1f 8b
    if (header[0] == 0x1f && header[1] == 0x8b) return 1;
    // BZIP2: BZ
    if (header[0] == 'B' && header[1] == 'Z') return 1;
    // JPEG: ff d8 ff
    if (len >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff) return 1;
    if (len < 4) return 0;
    // ELF: 7f 45 4c 46
    if (header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' && header[3] == 'F') return 1;
    // PDF: %PDF
    if (header[0] == '%' && header[1] == 'P' && header[2] == 'D' && header[3] == 'F') return 1;
    // PNG: 89 50 4e 47
    if (len >= 4 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') return 1;
    // ZIP/JAR/DOCX: 50 4b 03 04
    if (header[0] == 'P' && header[1] == 'K' && header[2] == 0x03 && header[3] == 0x04) return 1;
    // Java .class: ca fe ba be
    if (header[0] == 0xca && header[1] == 0xfe && header[2] == 0xba && header[3] == 0xbe) return 1;
    return 0;
}

// ============== Device file check ==============
static int is_device_file(const char *path) {
    if (!path) return 0;
    static const char *dev_paths[] = {
        "/dev/zero", "/dev/random", "/dev/urandom", "/dev/full",
        "/dev/stdin", "/dev/tty", "/dev/console",
        "/dev/stdout", "/dev/stderr",
        NULL
    };
    for (int i = 0; dev_paths[i]; i++) {
        if (strstr(path, dev_paths[i])) return 1;
    }
    if (strstr(path, "/proc/") && strstr(path, "/fd/")) return 1;
    return 0;
}

// ============== UNC path check ==============
static int is_unc_path(const char *path) {
    if (!path) return 0;
    if (path[0] == '\\' && path[1] == '\\') return 1;
#ifdef _WIN32
    if (path[0] == '/' && path[1] == '/') return 1;
#endif
    return 0;
}

// ============== Atomic file write ==============
static int write_file_atomically(const char *path, const char *content, size_t content_len) {
    // Generate temp file name: path + .tmp.<timestamp>
    size_t path_len = strlen(path);
    char *tmp_path = malloc(path_len + 32);
    if (!tmp_path) return -1;

#ifdef _WIN32
    snprintf(tmp_path, path_len + 32, "%s.tmp.%lld", path, (long long)GetTickCount64());
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(tmp_path, path_len + 32, "%s.tmp.%lld%09ld", path, (long long)ts.tv_sec, ts.tv_nsec);
#endif

    // Write to temp file
    FILE *f = utf8_fopen(tmp_path, "wb");
    if (!f) { free(tmp_path); return -1; }
    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);
    if (written != content_len) { remove(tmp_path); free(tmp_path); return -1; }

    // Rename temp to target (atomic on POSIX, best-effort on Windows)
#ifdef _WIN32
    // On Windows, MoveFileEx replaces the target atomically
    wchar_t *w_tmp = utf8_to_wide(tmp_path);
    wchar_t *w_dst = utf8_to_wide(path);
    int ok = 0;
    if (w_tmp && w_dst) {
        ok = MoveFileExW(w_tmp, w_dst, MOVEFILE_REPLACE_EXISTING) != 0;
    }
    free(w_tmp); free(w_dst);
    if (!ok) {
        // Fallback: direct write
        f = utf8_fopen(path, "wb");
        if (f) { fwrite(content, 1, content_len, f); fclose(f); ok = 1; }
        remove(tmp_path);
    }
#else
    if (rename(tmp_path, path) != 0) {
        // Fallback: direct write
        f = fopen(path, "wb");
        if (f) { fwrite(content, 1, content_len, f); fclose(f); }
        remove(tmp_path);
    }
#endif
    free(tmp_path);
    return 0;
}

// ============== Read tool ==============
#define MAX_FILE_SIZE (256 * 1024)       // 256KB
#define MAX_READ_OUTPUT (256 * 1024)     // 256KB output limit
#define DEFAULT_READ_LINES 2000          // default line limit (matching upstream)

char *tool_read_file(cJSON *input, char **error) {
    cJSON *path_item = cJSON_GetObjectItem(input, "file_path");
    if (!path_item || !path_item->valuestring) { *error = strdup("missing file_path"); return NULL; }

    char *fp = expand_path(path_item->valuestring);
    if (!fp) { *error = strdup("out of memory"); return NULL; }

    // Security: block UNC paths
    if (is_unc_path(fp)) {
        char *msg = malloc(strlen(fp) + 64);
        snprintf(msg, strlen(fp) + 64, "UNC path access deferred: %s", fp);
        free(fp); *error = msg; return NULL;
    }

    // Security: block device files
    if (is_device_file(fp)) {
        char *msg = malloc(strlen(fp) + 64);
        snprintf(msg, strlen(fp) + 64, "cannot read device file: %s", fp);
        free(fp); *error = msg; return NULL;
    }

    // Check file exists and get size
#ifdef _WIN32
    struct _stat st;
    if (_stat(fp, &st) != 0) { free(fp); *error = strdup("file not found"); return NULL; }
#else
    struct stat st;
    if (stat(fp, &st) != 0) { free(fp); *error = strdup("file not found"); return NULL; }
#endif
    if (S_ISDIR(st.st_mode)) { free(fp); *error = strdup("not a file (is a directory)"); return NULL; }

    // Binary extension check
    const char *dot = strrchr(fp, '.');
    if (dot && is_binary_extension(dot)) {
        char *msg = malloc(strlen(dot) + 64);
        snprintf(msg, strlen(dot) + 64, "binary file not supported: %s", dot);
        free(fp); *error = msg; return NULL;
    }

    // Parse offset/limit
    cJSON *offset_item = cJSON_GetObjectItem(input, "offset");
    cJSON *limit_item = cJSON_GetObjectItem(input, "limit");

    int has_explicit_offset = (offset_item && offset_item->type == cJSON_Number);
    int has_explicit_limit = (limit_item && limit_item->type == cJSON_Number);
    int is_partial_request = has_explicit_offset && has_explicit_limit;

    int line_offset = 0;
    int line_limit = DEFAULT_READ_LINES;
    if (has_explicit_offset) {
        int val = (int)offset_item->valuedouble;
        if (val < 1) val = 1;
        line_offset = val - 1;
    }
    if (has_explicit_limit) {
        int val = (int)limit_item->valuedouble;
        if (val > 0) line_limit = val;
    }

    // Only enforce file size limit for full-file reads
    if (!is_partial_request && st.st_size > MAX_FILE_SIZE) {
        free(fp); *error = strdup("file too large (>256 KB). Use offset and limit parameters to read specific portions."); return NULL;
    }

    // Read file
    FILE *f = utf8_fopen(fp, "rb");
    if (!f) { free(fp); *error = strdup("failed to open file"); return NULL; }

    long fsize = st.st_size;
    char *raw_content = malloc(fsize + 1);
    if (!raw_content) { fclose(f); free(fp); *error = strdup("out of memory"); return NULL; }
    size_t read_bytes = fread(raw_content, 1, fsize, f);
    raw_content[read_bytes] = '\0';
    fclose(f);

    // Magic bytes detection
    if (read_bytes >= 4 && is_binary_magic((const unsigned char *)raw_content, (int)read_bytes < 512 ? (int)read_bytes : 512)) {
        free(raw_content); free(fp); *error = strdup("binary file detected (magic bytes mismatch)"); return NULL;
    }

    // Strip BOM if present
    char *content_start = raw_content;
    if (read_bytes >= 3 && (unsigned char)raw_content[0] == 0xEF &&
        (unsigned char)raw_content[1] == 0xBB && (unsigned char)raw_content[2] == 0xBF) {
        content_start = raw_content + 3;
    }

    // Split into lines and output with cat -n format (lineNum\tcontent)
    Buffer lines_buf;
    buffer_init(&lines_buf);
    int total_file_lines = 0;
    int lines_read = 0;

    char *ptr = content_start;
    while (*ptr) {
        char *line_end = strchr(ptr, '\n');
        if (line_end) {
            *line_end = '\0';
            size_t line_len = strlen(ptr);
            if (line_len > 0 && ptr[line_len - 1] == '\r') ptr[--line_len] = '\0';

            total_file_lines++;
            if (total_file_lines > line_offset && lines_read < line_limit) {
                char prefix[32];
                snprintf(prefix, sizeof(prefix), "%d\t", total_file_lines);
                buffer_append_str(&lines_buf, prefix);
                buffer_append_str(&lines_buf, ptr);
                buffer_append_str(&lines_buf, "\n");
                lines_read++;
            }
            ptr = line_end + 1;
        } else {
            if (*ptr) {
                total_file_lines++;
                if (total_file_lines > line_offset && lines_read < line_limit) {
                    char prefix[32];
                    snprintf(prefix, sizeof(prefix), "%d\t", total_file_lines);
                    buffer_append_str(&lines_buf, prefix);
                    buffer_append_str(&lines_buf, ptr);
                    buffer_append_str(&lines_buf, "\n");
                    lines_read++;
                }
            }
            break;
        }
    }
    free(raw_content);

    // Empty file
    if (total_file_lines == 0) {
        free(fp);
        return strdup("<system-reminder>Warning: the file exists but the contents are empty.</system-reminder>");
    }

    // Offset beyond file
    if (line_offset >= total_file_lines) {
        char msg[256];
        snprintf(msg, sizeof(msg), "<system-reminder>Warning: the file exists but is shorter than the provided offset (%d). The file has %d lines.</system-reminder>", line_offset + 1, total_file_lines);
        free(fp);
        return strdup(msg);
    }

    char *result = lines_buf.data ? lines_buf.data : strdup("");
    lines_buf.data = NULL; buffer_free(&lines_buf);

    // Truncate output if exceeds byte limit
    size_t result_len = strlen(result);
    if (result_len > MAX_READ_OUTPUT) {
        result[MAX_READ_OUTPUT] = '\0';
        // Append truncation notice
        char *truncated = malloc(MAX_READ_OUTPUT + 128);
        if (truncated) {
            memcpy(truncated, result, MAX_READ_OUTPUT);
            strcpy(truncated + MAX_READ_OUTPUT, "\n... (output truncated due to size limit)");
            free(result);
            result = truncated;
        }
    }

    // Add pagination hint
    Buffer out_buf;
    buffer_init(&out_buf);
    buffer_append_str(&out_buf, result);
    free(result);

    int end_line = line_offset + lines_read;
    if (end_line < total_file_lines) {
        char suffix[256];
        snprintf(suffix, sizeof(suffix), "\n\n(Showing lines %d-%d of %d. Use offset=%d to continue.)", line_offset + 1, end_line, total_file_lines, end_line + 1);
        buffer_append_str(&out_buf, suffix);
    } else {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "\n\n(End of file - %d lines total)", total_file_lines);
        buffer_append_str(&out_buf, suffix);
    }

    char *output = out_buf.data ? out_buf.data : strdup("");
    out_buf.data = NULL; buffer_free(&out_buf);

    char *dirty = output;
    output = clean_utf8(dirty);
    free(dirty);
    free(fp);
    return output;
}

// ============== Write tool ==============
#define MAX_WRITE_SIZE (10 * 1024 * 1024)  // 10MB

char *tool_write_file(cJSON *input, char **error) {
    cJSON *path_item = cJSON_GetObjectItem(input, "file_path");
    cJSON *content = cJSON_GetObjectItem(input, "content");
    if (!path_item || !path_item->valuestring) { *error = strdup("missing file_path"); return NULL; }
    if (!content || !content->valuestring) { *error = strdup("missing content"); return NULL; }

    size_t content_len = strlen(content->valuestring);

    // Content size limit
    if (content_len > MAX_WRITE_SIZE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "content too large (%zu bytes, max %d bytes)", content_len, MAX_WRITE_SIZE);
        *error = strdup(msg); return NULL;
    }

    char *fp = expand_path(path_item->valuestring);
    if (!fp) { *error = strdup("out of memory"); return NULL; }

    // Security: block UNC paths
    if (is_unc_path(fp)) {
        char *msg = malloc(strlen(fp) + 64);
        snprintf(msg, strlen(fp) + 64, "UNC path access deferred: %s", fp);
        free(fp); *error = msg; return NULL;
    }

    // Create parent directories
    char dir_buf[4096];
    strncpy(dir_buf, fp, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *last_sep = strrchr(dir_buf, '/');
    if (!last_sep) last_sep = strrchr(dir_buf, '\\');
    if (last_sep) {
        *last_sep = '\0';
        mkdirs(dir_buf);
    }

    // Atomic write
    if (write_file_atomically(fp, content->valuestring, content_len) != 0) {
        free(fp); *error = strdup("failed to write file"); return NULL;
    }

    // Build result message
    char *result = malloc(64 + strlen(fp) + 20);
    if (result) sprintf(result, "Wrote %zu chars to %s", content_len, fp);
    else result = strdup("File written successfully");

    // Large file warning (>1MB)
    if (content_len > 1024 * 1024) {
        char *warn = malloc(strlen(result) + 128);
        if (warn) {
            double size_mb = (double)content_len / (1024.0 * 1024.0);
            snprintf(warn, strlen(result) + 128, "%s\n[WARN] Large file written (%.1f MB). Confirm with the user before proceeding.", result, size_mb);
            free(result);
            result = warn;
        }
    }

    free(fp);
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
    cJSON *path = cJSON_GetObjectItem(input, "file_path");
    cJSON *old_str = cJSON_GetObjectItem(input, "old_string");
    cJSON *new_str = cJSON_GetObjectItem(input, "new_string");
    cJSON *replace_all_item = cJSON_GetObjectItem(input, "replace_all");
    if (!path || !path->valuestring) { *error = strdup("missing file_path"); return NULL; }
    if (!old_str || !old_str->valuestring) { *error = strdup("missing old_string"); return NULL; }
    if (!new_str || !new_str->valuestring) { *error = strdup("missing new_string"); return NULL; }

    int replace_all = replace_all_item && replace_all_item->valueint != 0;

    // Expand ~ in path
    char *fp = expand_path(path->valuestring);
    if (!fp) { *error = strdup("out of memory"); return NULL; }

    // Block .ipynb files (complex JSON structure, easy to corrupt)
    const char *ext = strrchr(fp, '.');
    if (ext && strcmp(ext, ".ipynb") == 0) {
        free(fp); *error = strdup("editing .ipynb files is not supported. Use write_file to overwrite the entire notebook, or read_file to view the JSON structure and make targeted edits."); return NULL;
    }

    // Security: block UNC paths
    if (is_unc_path(fp)) {
        char *msg = malloc(strlen(fp) + 64);
        snprintf(msg, strlen(fp) + 64, "UNC path access deferred: %s", fp);
        free(fp); *error = msg; return NULL;
    }

    // Check file exists and get size
#ifdef _WIN32
    struct _stat st;
    if (_stat(fp, &st) != 0) { free(fp); *error = strdup("file not found"); return NULL; }
#else
    struct stat st;
    if (stat(fp, &st) != 0) { free(fp); *error = strdup("file not found"); return NULL; }
#endif
    if (S_ISDIR(st.st_mode)) { free(fp); *error = strdup("not a file (is a directory)"); return NULL; }

    // File size limit (1GB)
    if (st.st_size > 1024LL * 1024 * 1024) {
        free(fp); *error = strdup("file too large (>1GB) to edit"); return NULL;
    }

    // Read file
    FILE *f = utf8_fopen(fp, "rb");
    if (!f) { free(fp); *error = strdup("failed to open file"); return NULL; }

    long size = st.st_size;
    char *content = malloc(size + 1);
    if (!content) { fclose(f); free(fp); *error = strdup("out of memory"); return NULL; }
    size_t read_bytes = fread(content, 1, size, f);
    content[read_bytes] = '\0';
    fclose(f);

    // Binary magic bytes detection
    if (read_bytes >= 4 && is_binary_magic((const unsigned char *)content, (int)(read_bytes < 512 ? read_bytes : 512))) {
        free(content); free(fp); *error = strdup("binary file detected"); return NULL;
    }

    // Strip BOM
    char *file_content = content;
    int bom_len = 0;
    if (read_bytes >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        bom_len = 3; file_content = content + 3;
    }

    // Detect line ending style
    int has_crlf = 0;
    for (int i = 0; i < (int)(read_bytes - bom_len); i++) {
        if (file_content[i] == '\r' && i + 1 < (int)(read_bytes - bom_len) && file_content[i + 1] == '\n') { has_crlf = 1; break; }
    }

    // Normalize line endings to LF for processing
    char *normalized = normalize_line_endings(file_content);
    char *normalized_old = normalize_line_endings(old_str->valuestring);
    char *normalized_new = normalize_line_endings(new_str->valuestring);
    if (!normalized || !normalized_old || !normalized_new) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        *error = strdup("out of memory"); return NULL;
    }

    // Strip trailing whitespace from old/new strings (except .md files)
    // This helps LLM-generated edits that may have trailing spaces
    if (ext && strcmp(ext, ".md") != 0) {
        {
            size_t len = strlen(normalized_old);
            while (len > 0 && (normalized_old[len-1] == ' ' || normalized_old[len-1] == '\t')) {
                normalized_old[--len] = '\0';
            }
        }
        {
            size_t len = strlen(normalized_new);
            while (len > 0 && (normalized_new[len-1] == ' ' || normalized_new[len-1] == '\t')) {
                normalized_new[--len] = '\0';
            }
        }
    }

    // Identical old/new check
    if (strlen(normalized_old) == 0 || strcmp(normalized_old, normalized_new) == 0) {
        free(content); free(normalized); free(normalized_old); free(normalized_new);
        *error = strdup("old_string and new_string are identical, no changes needed"); return NULL;
    }

    // Curly quote normalization for matching: convert smart quotes to straight
    // " (U+201C) = \xe2\x80\x9c, " (U+201D) = \xe2\x80\x9d
    // ' (U+2018) = \xe2\x80\x98, ' (U+2019) = \xe2\x80\x99
    char *curly_old = strdup(normalized_old);
    if (curly_old) {
        char *q = curly_old;
        for (char *p = curly_old; *p; p++) {
            if ((unsigned char)*p == 0xe2 && p[1] && p[2]) {
                unsigned char seq3 = (unsigned char)p[2];
                if ((unsigned char)p[1] == 0x80) {
                    if (seq3 == 0x9c || seq3 == 0x9d) { *q++ = '"'; p += 2; continue; }
                    if (seq3 == 0x98 || seq3 == 0x99) { *q++ = '\''; p += 2; continue; }
                }
            }
            *q++ = *p;
        }
        *q = '\0';
    }
    // Search with curly-quote-normalized version
    char *search_str = curly_old ? curly_old : normalized_old;

    // Find first occurrence
    char *pos = strstr(normalized, search_str);
    if (!pos) {
        // Try case-insensitive fuzzy search as fallback
        int fuzzy_idx = fuzzy_find_text(normalized, search_str);
        if (fuzzy_idx >= 0) pos = normalized + fuzzy_idx;
    }
    if (!pos) {
        free(content); free(normalized); free(normalized_old); free(normalized_new); free(curly_old);
        // Build error with old_string preview
        char err_buf[512];
        size_t preview_len = strlen(search_str) < 100 ? strlen(search_str) : 100;
        snprintf(err_buf, sizeof(err_buf), "old_string not found in file. Searched for: \"%.*s\"", (int)preview_len, search_str);
        *error = strdup(err_buf); return NULL;
    }

    // Count occurrences
    int occurrences = 0;
    char *check_pos = normalized;
    while ((check_pos = strstr(check_pos, search_str)) != NULL) { occurrences++; check_pos++; }

    if (!replace_all && occurrences > 1) {
        free(content); free(normalized); free(normalized_old); free(normalized_new); free(curly_old);
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "Found %d occurrences of old_string. Use replace_all=true to replace all, or make old_string more specific.", occurrences);
        *error = strdup(err_buf); return NULL;
    }

    // Build new content
    Buffer new_content_buf;
    buffer_init(&new_content_buf);

    if (replace_all) {
        char *last = normalized;
        check_pos = normalized;
        int replaced = 0;
        while ((pos = strstr(check_pos, search_str)) != NULL) {
            // Append content before match
            buffer_append(&new_content_buf, last, pos - last);
            // Append new string
            buffer_append_str(&new_content_buf, normalized_new);
            replaced++;
            last = pos + strlen(search_str);
            check_pos = last;
        }
        // Append remaining
        buffer_append_str(&new_content_buf, last);
        (void)occurrences; // suppress warning
    } else {
        // Single replacement
        int match_offset = pos - normalized;
        buffer_append(&new_content_buf, normalized, match_offset);
        buffer_append_str(&new_content_buf, normalized_new);
        buffer_append_str(&new_content_buf, pos + strlen(search_str));
    }

    char *new_normalized = new_content_buf.data;
    new_content_buf.data = NULL;
    buffer_free(&new_content_buf);

    // Check if content actually changed
    if (strcmp(file_content, new_normalized) == 0) {
        free(content); free(normalized); free(normalized_old); free(normalized_new); free(curly_old); free(new_normalized);
        *error = strdup("No changes made - file content already matches"); return NULL;
    }

    // Restore line endings
    char *final_content = new_normalized;
    if (has_crlf) {
        char *restored = restore_line_endings(new_normalized, 1);
        if (restored) { free(new_normalized); final_content = restored; }
    }

    // Write atomically
    size_t final_len = strlen(final_content);
    if (write_file_atomically(fp, final_content, final_len) != 0) {
        free(content); free(normalized); free(normalized_old); free(normalized_new); free(curly_old); free(final_content);
        free(fp); *error = strdup("failed to write file"); return NULL;
    }

    // Build result message
    int replaced_count = replace_all ? occurrences : 1;
    char *result = malloc(256 + strlen(fp) + strlen(normalized_new));
    if (result) {
        if (replace_all) {
            snprintf(result, 256 + strlen(fp) + strlen(normalized_new),
                "File edited successfully (%d occurrences replaced)", replaced_count);
        } else {
            snprintf(result, 256 + strlen(fp) + strlen(normalized_new),
                "File edited successfully");
        }
    } else {
        result = strdup("File edited successfully");
    }

    free(content); free(normalized); free(normalized_old); free(normalized_new); free(curly_old); free(final_content);
    free(fp);
    return result;
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
    item = cJSON_GetObjectItem(input, "maxCount"); cfg.max_count = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 0;
    item = cJSON_GetObjectItem(input, "maxResults"); cfg.max_results = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 250;
    item = cJSON_GetObjectItem(input, "head_limit"); cfg.head_limit = (item && item->type == cJSON_Number) ? (int)item->valuedouble : 0;
    cfg.case_sensitive = 0; cfg.include_binary = 0; cfg.max_line_length = 500; cfg.output_mode = OUTPUT_CONTENT;

    item = cJSON_GetObjectItem(input, "outputMode");
    if (item && item->valuestring) {
        if (strcmp(item->valuestring, "files_with_matches") == 0) cfg.output_mode = OUTPUT_FILES;
        else if (strcmp(item->valuestring, "count") == 0) cfg.output_mode = OUTPUT_COUNT;
    }

    item = cJSON_GetObjectItem(input, "caseInsensitive");
    if (item && item->type == cJSON_True) cfg.case_sensitive = 0;
    if (cJSON_GetObjectItem(input, "-i") || cJSON_GetObjectItem(input, "ignore_case") || cJSON_GetObjectItem(input, "case_insensitive")) {
        cfg.case_sensitive = 0;
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
    p_item = cJSON_GetObjectItem(input, "head_limit"); cfg.head_limit = (p_item && p_item->type == cJSON_Number) ? (int)p_item->valuedouble : 0;
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

#ifdef _WIN32
// ============== Shell command escaping for bash -c ==============
static char *escape_bash_command(const char *cmd) {
    if (!cmd) return strdup("");
    // Count escape characters needed
    int extra = 0;
    for (const char *p = cmd; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '`' || *p == '$') extra++;
    }
    if (extra == 0) return strdup(cmd);

    char *escaped = malloc(strlen(cmd) + extra + 1);
    if (!escaped) return strdup(cmd);

    int j = 0;
    for (const char *p = cmd; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '`' || *p == '$') {
            escaped[j++] = '\\';
        }
        escaped[j++] = *p;
    }
    escaped[j] = '\0';
    return escaped;
}
#endif

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

    // Escape command for safe embedding in bash -c "..."
    char *escaped_cmd = escape_bash_command(command);

    char cmd_line[16384];
    if (work_dir) {
        // Convert Windows path like E:\workspace to /e/workspace
        char posix_dir[512]; int pi = 0;
        for (int ci = 0; work_dir[ci] && pi < (int)(sizeof(posix_dir)-1); ci++) {
            if (work_dir[ci] == '\\') posix_dir[pi++] = '/';
            else if (work_dir[ci] == ':') posix_dir[pi++] = '/';
            else posix_dir[pi++] = tolower((unsigned char)work_dir[ci]);
        }
        posix_dir[pi] = '\0';
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"cd %s && %s\"", bash_exe, posix_dir, escaped_cmd);
    } else {
        snprintf(cmd_line, sizeof(cmd_line), "\"%s\" -c \"%s\"", bash_exe, escaped_cmd);
    }
    free(escaped_cmd);

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

// ============================================================================
// TodoWrite tool (like Go project's tools/todo_write.go)
// ============================================================================

#define MAX_TODOS 64

typedef struct {
    char content[256];
    char status[16];    // "pending", "in_progress", "completed"
    char active_form[128];
} TodoItem;

typedef struct {
    TodoItem items[MAX_TODOS];
    int count;
    int turns_since_write;
    int turns_since_remind;
} TodoList;

static TodoList g_todo_list = {0};

static void todo_list_update(TodoItem *new_items, int new_count) {
    if (new_count > MAX_TODOS) new_count = MAX_TODOS;
    memcpy(g_todo_list.items, new_items, new_count * sizeof(TodoItem));
    g_todo_list.count = new_count;
    g_todo_list.turns_since_write = 0;
}

int todo_list_increment_turn(void) {
    g_todo_list.turns_since_write++;
    g_todo_list.turns_since_remind++;
    int should_remind = g_todo_list.turns_since_write >= 10 &&
                        g_todo_list.turns_since_remind >= 10;
    if (should_remind) {
        g_todo_list.turns_since_remind = 0;
    }
    return should_remind;
}

void todo_list_reset_write_counter(void) {
    g_todo_list.turns_since_write = 0;
}

char *todo_list_build_reminder(void) {
    if (g_todo_list.count == 0) return NULL;

    Buffer buf;
    buffer_init(&buf);
    buffer_append_str(&buf, SYSTEM_INJECTED_MARKER "\n## Current Tasks\n");

    for (int i = 0; i < g_todo_list.count; i++) {
        TodoItem *item = &g_todo_list.items[i];
        const char *icon = "o"; // pending
        if (strcmp(item->status, "in_progress") == 0) icon = "~";
        else if (strcmp(item->status, "completed") == 0) icon = "x";

        char line[512];
        if (item->active_form[0]) {
            snprintf(line, sizeof(line), "  %s %s (%s) [%s]\n",
                icon, item->content, item->active_form, item->status);
        } else {
            snprintf(line, sizeof(line), "  %s %s [%s]\n",
                icon, item->content, item->status);
        }
        buffer_append_str(&buf, line);
    }

    buffer_append_str(&buf, "\n## Important\nUse TodoWrite tool to keep the above task list up to date as you work.");
    return buffer_steal(&buf);
}

char *todo_list_build_idle_reminder(void) {
    return strdup(SYSTEM_INJECTED_MARKER
        "The TodoWrite tool hasn't been used recently. "
        "If you're on tasks that would benefit from tracking progress, "
        "consider using the TodoWrite tool to update your task list. "
        "If your current task list is stale, update it. "
        "If you don't have a task list, create one for multi-step work.");
}

char *tool_todo_write(cJSON *input, char **error) {
    (void)error;
    cJSON *todos = cJSON_GetObjectItem(input, "todos");
    if (!todos || todos->type != cJSON_Array) {
        *error = strdup("todos must be an array");
        return NULL;
    }

    TodoItem items[MAX_TODOS];
    int count = 0;
    int arr_size = cJSON_GetArraySize(todos);

    for (int i = 0; i < arr_size && count < MAX_TODOS; i++) {
        cJSON *item = cJSON_GetArrayItem(todos, i);
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *status = cJSON_GetObjectItem(item, "status");
        cJSON *active_form = cJSON_GetObjectItem(item, "activeForm");

        if (!content || !content->valuestring || !status || !status->valuestring) continue;

        // Validate status
        if (strcmp(status->valuestring, "pending") != 0 &&
            strcmp(status->valuestring, "in_progress") != 0 &&
            strcmp(status->valuestring, "completed") != 0) {
            continue;
        }

        memset(&items[count], 0, sizeof(TodoItem));
        strncpy(items[count].content, content->valuestring, sizeof(items[count].content) - 1);
        strncpy(items[count].status, status->valuestring, sizeof(items[count].status) - 1);
        if (active_form && active_form->valuestring) {
            strncpy(items[count].active_form, active_form->valuestring, sizeof(items[count].active_form) - 1);
        }
        count++;
    }

    todo_list_update(items, count);

    return strdup("Todos updated. Ensure that you use the todo list to track your progress. Please proceed with the current tasks as applicable");
}
