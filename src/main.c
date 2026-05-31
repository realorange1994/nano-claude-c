#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "config.h"
#include "provider.h"
#include "tool.h"
#include "repl.h"
#include "http.h"

static void print_help(const char *prog) {
    printf("NanoClaude-C - Pure C AI Agent\n");
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -h, --help        Show this help\n");
    printf("  -c, --config FILE  Config file path (default: ~/.nanoclaude/config.json)\n");
    printf("  -m, --model MODEL  Override model\n");
    printf("  --provider TYPE    Provider: anthropic (default) or openai\n");
    printf("  --api-key KEY     API key (or set ANTHROPIC_API_KEY env)\n");
    printf("  --base-url URL    Custom API base URL\n");
    printf("\nEnvironment Variables:\n");
    printf("  ANTHROPIC_API_KEY   Anthropic API key\n");
    printf("  OPENAI_API_KEY      OpenAI API key\n");
    printf("\nExample:\n");
    printf("  %s --api-key sk-ant-...\n", prog);
}

int main(int argc, char *argv[]) {
    // Setup console for UTF-8
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    const char *config_path = NULL;
    const char *model_override = NULL;
    const char *provider_override = NULL;
    const char *api_key_override = NULL;
    const char *base_url_override = NULL;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) config_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (i + 1 < argc) model_override = argv[++i];
        } else if (strcmp(argv[i], "--provider") == 0) {
            if (i + 1 < argc) provider_override = argv[++i];
        } else if (strcmp(argv[i], "--api-key") == 0) {
            if (i + 1 < argc) api_key_override = argv[++i];
        } else if (strcmp(argv[i], "--base-url") == 0) {
            if (i + 1 < argc) base_url_override = argv[++i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }
    
    // Load config
    if (!config_load(config_path)) {
        fprintf(stderr, "Warning: Using default config\n");
    }
    
    // Apply overrides
    ProviderType provider_type = PROVIDER_ANTHROPIC;
    if (provider_override) {
        if (strcmp(provider_override, "openai") == 0) {
            provider_type = PROVIDER_OPENAI;
        }
    } else {
        if (strcmp(g_config.provider, "openai") == 0) {
            provider_type = PROVIDER_OPENAI;
        }
    }
    
    const char *api_key = api_key_override;
    if (!api_key) {
        if (provider_type == PROVIDER_ANTHROPIC) {
            api_key = getenv("ANTHROPIC_API_KEY");
        } else {
            api_key = getenv("OPENAI_API_KEY");
        }
    }
    if (!api_key || !*api_key) {
        api_key = g_config.api_key;
    }
    
    if (!api_key || !*api_key) {
        fprintf(stderr, "Error: API key required. Set ANTHROPIC_API_KEY or --api-key\n");
        return 1;
    }
    
    const char *model = model_override ? model_override : g_config.model;
    const char *base_url = base_url_override ? base_url_override : g_config.base_url;
    
    // Initialize HTTP
    http_init();
    
    // Create provider
    Provider *provider = provider_new(provider_type, api_key, model, base_url);
    if (!provider) {
        fprintf(stderr, "Error: Failed to create provider\n");
        http_cleanup();
        return 1;
    }
    
    // Create tool registry
    ToolRegistry *tools = tool_registry_new();
    tool_register_builtins(tools);
    
    // Create REPL
    REPL *repl = repl_new(provider, tools);
    
    // Add MCP servers from config
    for (int i = 0; i < g_config.mcp_count; i++) {
        if (repl_add_mcp(repl, g_config.mcp_servers[i].name, g_config.mcp_servers[i].command)) {
            printf("Connected to MCP server: %s\n", g_config.mcp_servers[i].name);
        } else {
            fprintf(stderr, "Failed to connect to MCP server: %s\n", g_config.mcp_servers[i].name);
        }
    }
    
    // Run REPL
    int result = repl_run(repl);
    
    // Cleanup
    repl_free(repl);
    tool_registry_free(tools);
    provider_free(provider);
    http_cleanup();
    config_free();
    
    return result;
}
