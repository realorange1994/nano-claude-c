#include "jsonrpc.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <process.h>
#include <windows.h>

struct MCPClient {
    char *name;
    char *command;
    HANDLE stdin_fd;
    HANDLE stdout_fd;
    PROCESS_INFORMATION proc_info;
    int next_id;
    bool initialized;
};

static int next_id(MCPClient *mcp) {
    return mcp->next_id++;
}

static char *send_request(MCPClient *mcp, cJSON *request) {
    char *json_str = cJSON_Print(request);
    if (!json_str) return NULL;
    
    DWORD written;
    WriteFile(mcp->stdin_fd, json_str, (DWORD)strlen(json_str), &written, NULL);
    WriteFile(mcp->stdin_fd, "\n", 1, &written, NULL);
    
    free(json_str);
    
    // Read response
    Buffer buf;
    buffer_init(&buf);
    
    char chunk[4096];
    DWORD bytes_read;
    
    while (ReadFile(mcp->stdout_fd, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        buffer_append(&buf, chunk, bytes_read);
        
        // Check if we have complete JSON
        char *str = buffer_c_str(&buf);
        cJSON *resp = cJSON_Parse(str);
        if (resp) {
            char *result = cJSON_Print(resp);
            cJSON_Delete(resp);
            buffer_free(&buf);
            return result;
        }
    }
    
    buffer_free(&buf);
    return NULL;
}

MCPClient *mcp_client_new(const char *name, const char *command) {
    MCPClient *mcp = calloc(1, sizeof(MCPClient));
    if (!mcp) return NULL;
    
    mcp->name = strdup(name);
    mcp->command = strdup(command);
    mcp->next_id = 1;
    
    return mcp;
}

void mcp_client_free(MCPClient *mcp) {
    if (!mcp) return;
    
    if (mcp->stdin_fd) CloseHandle(mcp->stdin_fd);
    if (mcp->stdout_fd) CloseHandle(mcp->stdout_fd);
    if (mcp->proc_info.hProcess) CloseHandle(mcp->proc_info.hProcess);
    if (mcp->proc_info.hThread) CloseHandle(mcp->proc_info.hThread);
    
    free(mcp->name);
    free(mcp->command);
    free(mcp);
}

const char *mcp_client_name(MCPClient *mcp) {
    return mcp->name;
}

bool mcp_initialize(MCPClient *mcp) {
    if (!mcp || mcp->initialized) return true;
    
    // Create pipes for stdin/stdout
    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    HANDLE stdin_read, stdin_write;
    HANDLE stdout_read, stdout_write;
    
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) return false;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        return false;
    }
    
    // Set handle information for stdin_write and stdout_read to not be inherited
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    
    // Start the process
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    
    if (!CreateProcessA(NULL, mcp->command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &mcp->proc_info)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        CloseHandle(stdout_read); CloseHandle(stdout_write);
        return false;
    }
    
    mcp->stdin_fd = stdin_write;
    mcp->stdout_fd = stdout_read;
    
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    
    // Send initialize request
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("initialize"));
    cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(next_id(mcp)));
    
    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    
    if (!resp) return false;
    
    cJSON *resp_obj = cJSON_Parse(resp);
    free(resp);
    
    if (!resp_obj) return false;
    
    cJSON_Delete(resp_obj);
    mcp->initialized = true;
    
    return true;
}

cJSON *mcp_list_tools(MCPClient *mcp) {
    if (!mcp || !mcp->initialized) return NULL;
    
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/list"));
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(next_id(mcp)));
    
    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    
    if (!resp) return NULL;
    
    cJSON *result = cJSON_Parse(resp);
    free(resp);
    
    if (!result) return NULL;
    
    cJSON *tools = cJSON_GetObjectItem(result, "result");
    if (tools) {
        cJSON *tools_arr = cJSON_GetObjectItem(tools, "tools");
        if (tools_arr) {
            cJSON *copy = cJSON_Duplicate(tools_arr, 1);
            cJSON_Delete(result);
            return copy;
        }
    }
    
    cJSON_Delete(result);
    return NULL;
}

cJSON *mcp_call_tool(MCPClient *mcp, const char *tool_name, cJSON *arguments) {
    if (!mcp || !mcp->initialized) return NULL;
    
    cJSON *req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(req, "method", cJSON_CreateString("tools/call"));
    
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "name", cJSON_CreateString(tool_name));
    if (arguments) {
        cJSON_AddItemToObject(params, "arguments", cJSON_Duplicate(arguments, 1));
    } else {
        cJSON_AddItemToObject(params, "arguments", cJSON_CreateObject());
    }
    cJSON_AddItemToObject(req, "params", params);
    cJSON_AddItemToObject(req, "id", cJSON_CreateNumber(next_id(mcp)));
    
    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    
    if (!resp) return NULL;
    
    cJSON *result = cJSON_Parse(resp);
    free(resp);
    
    if (!result) return NULL;
    
    cJSON *call_result = cJSON_GetObjectItem(result, "result");
    if (call_result) {
        cJSON *copy = cJSON_Duplicate(call_result, 1);
        cJSON_Delete(result);
        return copy;
    }
    
    cJSON_Delete(result);
    return NULL;
}

bool mcp_is_connected(MCPClient *mcp) {
    if (!mcp) return false;
    if (!mcp->initialized) return false;
    if (WaitForSingleObject(mcp->proc_info.hProcess, 0) == WAIT_TIMEOUT) {
        return true;
    }
    return false;
}
