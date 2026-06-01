#include "jsonrpc.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// MCP timeout in milliseconds
#define MCP_TIMEOUT_MS 30000

#ifdef _WIN32
#include <windows.h>
#include <process.h>

struct MCPClient {
    char *name;
    char *command;
    HANDLE stdin_fd;
    HANDLE stdout_fd;
    PROCESS_INFORMATION proc_info;
    int next_id;
    bool initialized;
};

#else
// Linux: POSIX implementation
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

struct MCPClient {
    char *name;
    char *command;
    int stdin_fd;
    int stdout_fd;
    pid_t pid;
    int next_id;
    bool initialized;
};
#endif

static int next_id(MCPClient *mcp) { return mcp->next_id++; }

#ifdef _WIN32
// ------ Windows: GetTickCount ------
static DWORD get_ticks(void) { return GetTickCount(); }

// ------ Windows: read response ------
static char *read_response_with_timeout(MCPClient *mcp) {
    Buffer buf;
    buffer_init(&buf);
    DWORD start_time = get_ticks();
    char chunk[4096];
    DWORD bytes_read;

    while (get_ticks() - start_time < MCP_TIMEOUT_MS) {
        if (WaitForSingleObject(mcp->proc_info.hProcess, 0) != WAIT_TIMEOUT) {
            buffer_free(&buf); return NULL;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(mcp->stdout_fd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            Sleep(50); continue;
        }
        if (ReadFile(mcp->stdout_fd, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
            bool valid = true;
            for (DWORD i = 0; i < bytes_read; i++) {
                unsigned char c = (unsigned char)chunk[i];
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { valid = false; break; }
            }
            if (!valid) continue;
            chunk[bytes_read] = '\0';
            buffer_append(&buf, chunk, bytes_read);
            char *str = buffer_c_str(&buf);
            size_t slen = strlen(str);
            char *first_brace = strchr(str, '{');
            if (!first_brace) { if (slen > 1024) { buf.data[0] = '\0'; buf.len = 0; } continue; }
            int depth = 0, in_string = 0, escaped = 0;
            for (char *p = first_brace; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (escaped) { escaped = 0; continue; }
                if (c == '\\' && in_string) { escaped = 1; continue; }
                if (c == '"') { in_string = !in_string; continue; }
                if (in_string) continue;
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        size_t json_len = (size_t)(p - first_brace) + 1;
                        char *complete = malloc(json_len + 1);
                        if (complete) { memcpy(complete, first_brace, json_len); complete[json_len] = '\0'; buffer_free(&buf); return complete; }
                        goto done;
                    }
                }
            }
        } else { break; }
    }
done:
    buffer_free(&buf); return NULL;
}

static char *send_request(MCPClient *mcp, cJSON *request) {
    char *json_str = cJSON_PrintUnformatted(request);
    if (!json_str) return NULL;
    DWORD written;
    WriteFile(mcp->stdin_fd, json_str, (DWORD)strlen(json_str), &written, NULL);
    WriteFile(mcp->stdin_fd, "\n", 1, &written, NULL);
    FlushFileBuffers(mcp->stdin_fd);
    free(json_str);
    return read_response_with_timeout(mcp);
}

MCPClient *mcp_client_new(const char *name, const char *command) {
    MCPClient *mcp = calloc(1, sizeof(MCPClient));
    if (!mcp) return NULL;
    mcp->name = name ? strdup(name) : NULL;
    mcp->command = command ? strdup(command) : NULL;
    if (!mcp->name || !mcp->command) { free(mcp->name); free(mcp->command); free(mcp); return NULL; }
    mcp->next_id = 1;
    return mcp;
}

void mcp_client_free(MCPClient *mcp) {
    if (!mcp) return;
    if (mcp->stdin_fd) CloseHandle(mcp->stdin_fd);
    if (mcp->stdout_fd) CloseHandle(mcp->stdout_fd);
    if (mcp->proc_info.hProcess) { CloseHandle(mcp->proc_info.hProcess); }
    if (mcp->proc_info.hThread) { CloseHandle(mcp->proc_info.hThread); }
    free(mcp->name); free(mcp->command); free(mcp);
}

const char *mcp_client_name(MCPClient *mcp) { return mcp->name; }

bool mcp_initialize(MCPClient *mcp) {
    if (!mcp || mcp->initialized) return true;

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;

    HANDLE stdin_read, stdin_write;
    HANDLE stdout_read, stdout_write;
    HANDLE stderr_read, stderr_write;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) return false;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) { CloseHandle(stdin_read); CloseHandle(stdin_write); return false; }
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) { CloseHandle(stdin_read); CloseHandle(stdin_write); CloseHandle(stdout_read); CloseHandle(stdout_write); return false; }

    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read; si.hStdOutput = stdout_write; si.hStdError = stderr_write;

    size_t cmd_len = strlen(mcp->command) + 32;
    char *wrapped_cmd = malloc(cmd_len);
    if (wrapped_cmd) { snprintf(wrapped_cmd, cmd_len, "cmd.exe /c %s", mcp->command); free(mcp->command); mcp->command = wrapped_cmd; }

    if (!CreateProcessA(NULL, mcp->command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &mcp->proc_info)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        CloseHandle(stdout_read); CloseHandle(stdout_write);
        CloseHandle(stderr_read); CloseHandle(stderr_write);
        return false;
    }

    mcp->stdin_fd = stdin_write; mcp->stdout_fd = stdout_read;
    CloseHandle(stdin_read); CloseHandle(stdout_write);
    CloseHandle(stderr_read); CloseHandle(stderr_write);

    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("initialize"));
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "protocolVersion", cJSON_CreateString("2024-11-05"));
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON *clientInfo = cJSON_CreateObject();
    cJSON_AddItemToObject(clientInfo, "name", cJSON_CreateString("nanoclaude"));
    cJSON_AddItemToObject(clientInfo, "version", cJSON_CreateString("1.0.0"));
    cJSON_AddItemToObject(params, "clientInfo", clientInfo);
    cJSON_AddItemToObject(req, "params", params);

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) { fprintf(stderr, "[MCP] initialize: no response from %s\n", mcp->name); return false; }
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return false;
    cJSON *id = cJSON_GetObjectItem(resp_obj, "id");
    if (!id || !(id->type == cJSON_Number ? (int)id->valuedouble == req_id : 0)) { cJSON_Delete(resp_obj); return false; }
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return false; }
    cJSON_Delete(resp_obj);
    mcp->initialized = true;
    return true;
}

cJSON *mcp_list_tools(MCPClient *mcp) {
    if (!mcp || !mcp->initialized) return NULL;
    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/list"));
    cJSON_AddItemToObject(req, "params", cJSON_CreateObject());

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) return NULL;
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return NULL;
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return NULL; }
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) {
        cJSON *tools_arr = cJSON_GetObjectItem(result, "tools");
        if (tools_arr) { cJSON *copy = cJSON_Duplicate(tools_arr, 1); cJSON_Delete(resp_obj); return copy; }
    }
    cJSON_Delete(resp_obj);
    return NULL;
}

cJSON *mcp_call_tool(MCPClient *mcp, const char *tool_name, cJSON *arguments) {
    if (!mcp || !mcp->initialized) return NULL;
    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/call"));
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "name", cJSON_CreateString(tool_name));
    cJSON_AddItemToObject(params, "arguments", arguments ? cJSON_Duplicate(arguments, 1) : cJSON_CreateObject());
    cJSON_AddItemToObject(req, "params", params);

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) return NULL;
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return NULL;
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return NULL; }
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) { cJSON *copy = cJSON_Duplicate(result, 1); cJSON_Delete(resp_obj); return copy; }
    cJSON_Delete(resp_obj);
    return NULL;
}

bool mcp_is_connected(MCPClient *mcp) {
    if (!mcp || !mcp->initialized) return false;
    return WaitForSingleObject(mcp->proc_info.hProcess, 0) == WAIT_TIMEOUT;
}

#else
// ------ Linux: POSIX implementation ------

static int get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *read_response_with_timeout(MCPClient *mcp) {
    Buffer buf;
    buffer_init(&buf);
    int start_time = get_ticks_ms();
    char chunk[4096];

    while (get_ticks_ms() - start_time < MCP_TIMEOUT_MS) {
        // Check if process is alive
        pid_t result = waitpid(mcp->pid, NULL, WNOHANG);
        if (result > 0) { buffer_free(&buf); return NULL; }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(mcp->stdout_fd, &fds);
        struct timeval tv = {0, 50000}; // 50ms
        int ready = select(mcp->stdout_fd + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        ssize_t bytes_read = read(mcp->stdout_fd, chunk, sizeof(chunk) - 1);
        if (bytes_read > 0) {
            bool valid = true;
            for (int i = 0; i < bytes_read; i++) {
                unsigned char c = (unsigned char)chunk[i];
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { valid = false; break; }
            }
            if (!valid) continue;
            chunk[bytes_read] = '\0';
            buffer_append(&buf, chunk, bytes_read);
            char *str = buffer_c_str(&buf);
            size_t slen = strlen(str);
            char *first_brace = strchr(str, '{');
            if (!first_brace) { if (slen > 1024) { buf.data[0] = '\0'; buf.len = 0; } continue; }
            int depth = 0, in_string = 0, escaped = 0;
            for (char *p = first_brace; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (escaped) { escaped = 0; continue; }
                if (c == '\\' && in_string) { escaped = 1; continue; }
                if (c == '"') { in_string = !in_string; continue; }
                if (in_string) continue;
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        size_t json_len = (size_t)(p - first_brace) + 1;
                        char *complete = malloc(json_len + 1);
                        if (complete) { memcpy(complete, first_brace, json_len); complete[json_len] = '\0'; buffer_free(&buf); return complete; }
                        goto done;
                    }
                }
            }
        } else { break; }
    }
done:
    buffer_free(&buf); return NULL;
}

static char *send_request(MCPClient *mcp, cJSON *request) {
    char *json_str = cJSON_PrintUnformatted(request);
    if (!json_str) return NULL;
    dprintf(mcp->stdin_fd, "%s\n", json_str);
    free(json_str);
    return read_response_with_timeout(mcp);
}

MCPClient *mcp_client_new(const char *name, const char *command) {
    MCPClient *mcp = calloc(1, sizeof(MCPClient));
    if (!mcp) return NULL;
    mcp->name = name ? strdup(name) : NULL;
    mcp->command = command ? strdup(command) : NULL;
    if (!mcp->name || !mcp->command) { free(mcp->name); free(mcp->command); free(mcp); return NULL; }
    mcp->next_id = 1;
    return mcp;
}

void mcp_client_free(MCPClient *mcp) {
    if (!mcp) return;
    if (mcp->stdin_fd >= 0) { close(mcp->stdin_fd); mcp->stdin_fd = -1; }
    if (mcp->stdout_fd >= 0) { close(mcp->stdout_fd); mcp->stdout_fd = -1; }
    if (mcp->pid > 0) { kill(mcp->pid, SIGTERM); waitpid(mcp->pid, NULL, WNOHANG); }
    free(mcp->name); free(mcp->command); free(mcp);
}

const char *mcp_client_name(MCPClient *mcp) { return mcp->name; }

bool mcp_initialize(MCPClient *mcp) {
    if (!mcp || mcp->initialized) return true;

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        return false;
    }

    mcp->pid = fork();
    if (mcp->pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return false;
    }

    if (mcp->pid == 0) {
        // Child
        close(stdin_pipe[0]); close(stdout_pipe[1]); close(stderr_pipe[1]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[0], STDOUT_FILENO);
        dup2(stderr_pipe[0], STDERR_FILENO);
        close(stdin_pipe[0]); close(stdout_pipe[0]); close(stderr_pipe[0]);
        execl("/bin/sh", "sh", "-c", mcp->command, (char *)NULL);
        _exit(127);
    }

    // Parent
    close(stdin_pipe[0]); close(stdout_pipe[0]); close(stderr_pipe[0]); close(stderr_pipe[1]);

    mcp->stdin_fd = stdin_pipe[1];
    mcp->stdout_fd = stdout_pipe[0];

    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("initialize"));
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "protocolVersion", cJSON_CreateString("2024-11-05"));
    cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
    cJSON *clientInfo = cJSON_CreateObject();
    cJSON_AddItemToObject(clientInfo, "name", cJSON_CreateString("nanoclaude"));
    cJSON_AddItemToObject(clientInfo, "version", cJSON_CreateString("1.0.0"));
    cJSON_AddItemToObject(params, "clientInfo", clientInfo);
    cJSON_AddItemToObject(req, "params", params);

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) { fprintf(stderr, "[MCP] initialize: no response from %s\n", mcp->name); return false; }
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return false;
    cJSON *id = cJSON_GetObjectItem(resp_obj, "id");
    if (!id || !(id->type == cJSON_Number ? (int)id->valuedouble == req_id : 0)) { cJSON_Delete(resp_obj); return false; }
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return false; }
    cJSON_Delete(resp_obj);
    mcp->initialized = true;
    return true;
}

cJSON *mcp_list_tools(MCPClient *mcp) {
    if (!mcp || !mcp->initialized) return NULL;
    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/list"));
    cJSON_AddItemToObject(req, "params", cJSON_CreateObject());

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) return NULL;
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return NULL;
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return NULL; }
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) {
        cJSON *tools_arr = cJSON_GetObjectItem(result, "tools");
        if (tools_arr) { cJSON *copy = cJSON_Duplicate(tools_arr, 1); cJSON_Delete(resp_obj); return copy; }
    }
    cJSON_Delete(resp_obj);
    return NULL;
}

cJSON *mcp_call_tool(MCPClient *mcp, const char *tool_name, cJSON *arguments) {
    if (!mcp || !mcp->initialized) return NULL;
    int req_id = next_id(mcp);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(req_id));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/call"));
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "name", cJSON_CreateString(tool_name));
    cJSON_AddItemToObject(params, "arguments", arguments ? cJSON_Duplicate(arguments, 1) : cJSON_CreateObject());
    cJSON_AddItemToObject(req, "params", params);

    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    if (!resp) return NULL;
    cJSON *resp_obj = cJSON_Parse(resp); free(resp);
    if (!resp_obj) return NULL;
    cJSON *err = cJSON_GetObjectItem(resp_obj, "error");
    if (err) { cJSON_Delete(resp_obj); return NULL; }
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) { cJSON *copy = cJSON_Duplicate(result, 1); cJSON_Delete(resp_obj); return copy; }
    cJSON_Delete(resp_obj);
    return NULL;
}

bool mcp_is_connected(MCPClient *mcp) {
    if (!mcp || !mcp->initialized) return false;
    return waitpid(mcp->pid, NULL, WNOHANG) == 0;
}
#endif // !_WIN32
