#include "config.h"
#include "../deps/cJSON/cJSON.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

Config g_config;

static char *config_path = NULL;

char *config_default_path(void) {
    if (config_path) return config_path;
    
    const char *home = getenv("HOME");
    if (!home) {
        home = getenv("USERPROFILE");
    }
    if (!home) {
        home = "/tmp";
    }
    
    size_t len = strlen(home) + 32;
    config_path = malloc(len);
    if (!config_path) return NULL;
    
    snprintf(config_path, len, "%s/.nanoclaude/config.json", home);
    return config_path;
}

bool config_load(const char *path) {
    FILE *f;
    char *config_file;
    
    if (path) {
        config_file = (char*)path;
    } else {
        config_file = config_default_path();
    }
    
    if (!config_file) return false;
    
    f = fopen(config_file, "r");
    if (!f) {
        fprintf(stderr, "Warning: config file not found: %s\n", config_file);
        return false;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return false;
    }
    
    fread(content, 1, fsize, f);
    content[fsize] = '\0';
    fclose(f);
    
    // Parse JSON
    cJSON *root = cJSON_Parse(content);
    free(content);
    
    if (!root) {
        fprintf(stderr, "Error: failed to parse config file\n");
        return false;
    }
    
    // Parse fields
    cJSON *item;
    
    if ((item = cJSON_GetObjectItem(root, "provider"))) {
        strncpy(g_config.provider, item->valuestring, sizeof(g_config.provider) - 1);
    } else {
        strcpy(g_config.provider, "anthropic");
    }
    
    if ((item = cJSON_GetObjectItem(root, "api_key"))) {
        strncpy(g_config.api_key, item->valuestring, sizeof(g_config.api_key) - 1);
    }
    
    if ((item = cJSON_GetObjectItem(root, "base_url"))) {
        strncpy(g_config.base_url, item->valuestring, sizeof(g_config.base_url) - 1);
    } else {
        g_config.base_url[0] = '\0';
    }
    
    if ((item = cJSON_GetObjectItem(root, "model"))) {
        strncpy(g_config.model, item->valuestring, sizeof(g_config.model) - 1);
    } else {
        if (strcmp(g_config.provider, "anthropic") == 0) {
            strcpy(g_config.model, "claude-sonnet-4-20250514");
        } else {
            strcpy(g_config.model, "gpt-4o");
        }
    }
    
    if ((item = cJSON_GetObjectItem(root, "max_tokens"))) {
        g_config.max_tokens = item->valueint;
    } else {
        g_config.max_tokens = 8192;
    }
    
    if ((item = cJSON_GetObjectItem(root, "context_window"))) {
        g_config.context_window = item->valueint;
    } else {
        g_config.context_window = 200000;
    }
    
    if ((item = cJSON_GetObjectItem(root, "reserve_tokens"))) {
        g_config.reserve_tokens = item->valueint;
    } else {
        g_config.reserve_tokens = 16384;
    }
    
    if ((item = cJSON_GetObjectItem(root, "keep_recent_tokens"))) {
        g_config.keep_recent_tokens = item->valueint;
    } else {
        g_config.keep_recent_tokens = 20000;
    }
    
    // Parse MCP servers
    g_config.mcp_count = 0;
    cJSON *mcp_servers = cJSON_GetObjectItem(root, "mcp_servers");
    if (mcp_servers && mcp_servers->type == cJSON_Array) {
        int count = cJSON_GetArraySize(mcp_servers);
        for (int i = 0; i < count && i < MAX_MCP_SERVERS; i++) {
            cJSON *srv = cJSON_GetArrayItem(mcp_servers, i);
            cJSON *name = cJSON_GetObjectItem(srv, "name");
            cJSON *cmd = cJSON_GetObjectItem(srv, "command");
            
            if (name && cmd) {
                strncpy(g_config.mcp_servers[i].name, name->valuestring, 127);
                strncpy(g_config.mcp_servers[i].command, cmd->valuestring, 511);
                g_config.mcp_count++;
            }
        }
    }
    
    cJSON_Delete(root);
    return true;
}

void config_free(void) {
    if (config_path) {
        free(config_path);
        config_path = NULL;
    }
}
