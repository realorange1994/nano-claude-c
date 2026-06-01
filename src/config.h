#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define MAX_MCP_SERVERS 16
#define MAX_PATH_LEN 1024

typedef struct {
    char name[128];
    char command[512];
} MCPServer;

typedef struct {
    char provider[32];          // "anthropic" or "openai"
    char api_key[256];
    char base_url[512];         // Custom API URL (optional)
    char model[128];
    int context_window;
    int reserve_tokens;
    int keep_recent_tokens;
    MCPServer mcp_servers[MAX_MCP_SERVERS];
    int mcp_count;
} Config;

// Global config
extern Config g_config;

// Load config from file
bool config_load(const char *path);
void config_free(void);

// Get config path (~/.nanoclaude/config.json)
char *config_default_path(void);

#endif // CONFIG_H
