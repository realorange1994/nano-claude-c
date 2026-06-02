#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "provider.h"
#include "tool.h"
#include "repl.h"
#include "http.h"

#ifdef _WIN32
#include <windows.h>

static LONG WINAPI global_crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    fprintf(stderr, "\n[CRASH] Exception code: 0x%08X\n", code);
    fprintf(stderr, "[CRASH] Address: %p\n", (void*)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    printf("\n[Process crashed: exception 0x%08X]\n", code);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
#include <signal.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#define HAS_BACKTRACE
#endif

static void global_crash_handler(int sig) {
/*     void *stack[64]; */
/*     int n = backtrace(stack, 64); */
/*     fprintf(stderr, "\n[CRASH] Signal %d (%s)\n", sig, */
/*             sig == SIGSEGV ? "SIGSEGV" : */
/*             sig == SIGABRT ? "SIGABRT" : */
/*             sig == SIGBUS  ? "SIGBUS" : */
/*             sig == SIGFPE  ? "SIGFPE" : */
/*             sig == SIGILL  ? "SIGILL" : "unknown"); */
/*     backtrace_symbols_fd(stack, n, 2); */
    fflush(stderr);
    printf("\n[Process crashed: signal %d]\n", sig);
    fflush(stdout);
    _exit(1);
}
#endif

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
    printf("  --max-tokens N    Max response tokens (default: 8192)\n");
    printf("\nEnvironment Variables:\n");
    printf("  ANTHROPIC_API_KEY   Anthropic API key\n");
    printf("  OPENAI_API_KEY      OpenAI API key\n");
    printf("\nExample:\n");
    printf("  %s --api-key sk-ant-...\n", prog);
}

int g_verbose = 0;  // Global verbose flag, set by -v

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(global_crash_handler);
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#else
    signal(SIGSEGV, global_crash_handler);
    signal(SIGABRT, global_crash_handler);
    signal(SIGBUS, global_crash_handler);
    signal(SIGFPE, global_crash_handler);
    signal(SIGILL, global_crash_handler);
#endif

    const char *config_path = NULL;
    const char *model_override = NULL;
    const char *provider_override = NULL;
    const char *api_key_override = NULL;
    const char *base_url_override = NULL;
    int max_tokens_override = 0;

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
        } else if (strcmp(argv[i], "--max-tokens") == 0) {
            if (i + 1 < argc) max_tokens_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    if (!config_load(config_path)) {
        fprintf(stderr, "Warning: Using default config\n");
    }

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
    int max_tokens = max_tokens_override > 0 ? max_tokens_override : g_config.max_tokens;

    http_init();

    Provider *provider = provider_new(provider_type, api_key, model, base_url, max_tokens);
    if (!provider) {
        fprintf(stderr, "Error: Failed to create provider\n");
        http_cleanup();
        return 1;
    }

    ToolRegistry *tools = tool_registry_new();
    tool_register_builtins(tools);

    REPL *repl = repl_new(provider, tools);

    for (int i = 0; i < g_config.mcp_count; i++) {
        if (repl_add_mcp(repl, tools, g_config.mcp_servers[i].name, g_config.mcp_servers[i].command)) {
            printf("Connected to MCP server: %s\n", g_config.mcp_servers[i].name);
        } else {
            fprintf(stderr, "Failed to connect to MCP server: %s\n", g_config.mcp_servers[i].name);
        }
    }

    int result = repl_run(repl);

    repl_free(repl);
    tool_registry_free(tools);
    provider_free(provider);
    http_cleanup();
    config_free();

    return result;
}
