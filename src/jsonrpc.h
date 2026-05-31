#ifndef JSONRPC_H
#define JSONRPC_H

#include <stdbool.h>
#include <pthread.h>
#include "cJSON.h"

// MCP Client handle
typedef struct MCPClient MCPClient;

// Create/destroy MCP client
MCPClient *mcp_client_new(const char *name, const char *command);
void mcp_client_free(MCPClient *mcp);

// Get client name
const char *mcp_client_name(MCPClient *mcp);

// Initialize connection (send initialize request)
bool mcp_initialize(MCPClient *mcp);

// List available tools
cJSON *mcp_list_tools(MCPClient *mcp);

// Call a tool
cJSON *mcp_call_tool(MCPClient *mcp, const char *tool_name, cJSON *arguments);

// Check if client is connected
bool mcp_is_connected(MCPClient *mcp);

#endif // JSONRPC_H
