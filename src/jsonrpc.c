#include "jsonrpc.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <process.h>
#include <windows.h>

// MCP timeout in milliseconds
#define MCP_TIMEOUT_MS 30000

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

// Read response with timeout
static char *read_response_with_timeout(MCPClient *mcp) {
    Buffer buf;
    buffer_init(&buf);
    
    DWORD start_time = GetTickCount();
    char chunk[4096];
    DWORD bytes_read;
    
    while (GetTickCount() - start_time < MCP_TIMEOUT_MS) {
        // Check if process is still alive
        if (WaitForSingleObject(mcp->proc_info.hProcess, 0) != WAIT_TIMEOUT) {
            buffer_free(&buf);
            return NULL;  // Process died
        }
        
        // Try to read with small timeout
        DWORD avail = 0;
        if (!PeekNamedPipe(mcp->stdout_fd, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            Sleep(10);  // No data, wait a bit
            continue;
        }
        
        if (ReadFile(mcp->stdout_fd, chunk, sizeof(chunk) - 1, &bytes_read, NULL) && bytes_read > 0) {
            chunk[bytes_read] = '\0';
            buffer_append(&buf, chunk, bytes_read);
            
            // Check if we have complete JSON (look for newline or complete object)
            char *str = buffer_c_str(&buf);
            
            // Strip "data: " prefix if present
            if (strncmp(str, "data: ", 6) == 0) {
                memmove(str, str + 6, strlen(str + 6) + 1);
            }
            
            // Try to parse JSON
            cJSON *resp = cJSON_Parse(str);
            if (resp) {
                char *result = cJSON_Print(resp);
                cJSON_Delete(resp);
                buffer_free(&buf);
                return result;
            }
            
            // Check if we have complete JSON object (even if partial parsing fails)
            // Look for balanced braces
            int brace_count = 0;
            for (size_t i = 0; i < strlen(str); i++) {
                if (str[i] == '{') brace_count++;
                else if (str[i] == '}') brace_count--;
                if (brace_count == 0 && i > 0) {
                    // Complete JSON object found
                    char *complete = malloc(i + 2);
                    if (complete) {
                        strncpy(complete, str, i + 1);
                        complete[i + 1] = '\0';
                        buffer_free(&buf);
                        return complete;
                    }
                }
            }
        } else {
            break;  // Read error
        }
    }
    
    buffer_free(&buf);
    return NULL;  // Timeout
}

static char *send_request(MCPClient *mcp, cJSON *request) {
    char *json_str = cJSON_Print(request);
    if (!json_str) return NULL;
    
    DWORD written;
    WriteFile(mcp->stdin_fd, json_str, (DWORD)strlen(json_str), &written, NULL);
    WriteFile(mcp->stdin_fd, "\n", 1, &written, NULL);
    FlushFileBuffers(mcp->stdin_fd);
    
    free(json_str);
    
    // Read response with timeout
    return read_response_with_timeout(mcp);
}

// Check if response ID matches expected ID
static bool check_response_id(cJSON *response, int expected_id) {
    cJSON *id = cJSON_GetObjectItem(response, "id");
    if (!id) return false;
    
    // ID can be number or string
    if (id->type == cJSON_Number) {
        return (int)id->valuedouble == expected_id;
    } else if (id->type == cJSON_String) {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", expected_id);
        return strcmp(id->valuestring, id_str) == 0;
    }
    
    return false;
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
    
    // Build initialize request (matching Go version)
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
    
    if (!resp) return false;
    
    cJSON *resp_obj = cJSON_Parse(resp);
    free(resp);
    
    if (!resp_obj) return false;
    
    // Check response ID
    if (!check_response_id(resp_obj, req_id)) {
        cJSON_Delete(resp_obj);
        return false;
    }
    
    // Check for error
    cJSON *error = cJSON_GetObjectItem(resp_obj, "error");
    if (error) {
        cJSON_Delete(resp_obj);
        return false;
    }
    
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
    
    cJSON *resp_obj = cJSON_Parse(resp);
    free(resp);
    
    if (!resp_obj) return NULL;
    
    // Check response ID
    if (!check_response_id(resp_obj, req_id)) {
        cJSON_Delete(resp_obj);
        return NULL;
    }
    
    // Check for error
    cJSON *error = cJSON_GetObjectItem(resp_obj, "error");
    if (error) {
        cJSON_Delete(resp_obj);
        return NULL;
    }
    
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) {
        cJSON *tools_arr = cJSON_GetObjectItem(result, "tools");
        if (tools_arr) {
            cJSON *copy = cJSON_Duplicate(tools_arr, 1);
            cJSON_Delete(resp_obj);
            return copy;
        }
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
    if (arguments) {
        cJSON_AddItemToObject(params, "arguments", cJSON_Duplicate(arguments, 1));
    } else {
        cJSON_AddItemToObject(params, "arguments", cJSON_CreateObject());
    }
    cJSON_AddItemToObject(req, "params", params);
    
    char *resp = send_request(mcp, req);
    cJSON_Delete(req);
    
    if (!resp) return NULL;
    
    cJSON *resp_obj = cJSON_Parse(resp);
    free(resp);
    
    if (!resp_obj) return NULL;
    
    // Check response ID
    if (!check_response_id(resp_obj, req_id)) {
        cJSON_Delete(resp_obj);
        return NULL;
    }
    
    // Check for error
    cJSON *error = cJSON_GetObjectItem(resp_obj, "error");
    if (error) {
        cJSON_Delete(resp_obj);
        return NULL;
    }
    
    cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
    if (result) {
        cJSON *copy = cJSON_Duplicate(result, 1);
        cJSON_Delete(resp_obj);
        return copy;
    }
    
    cJSON_Delete(resp_obj);
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
