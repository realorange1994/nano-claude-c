#ifndef REPL_H
#define REPL_H

#include "provider.h"
#include "tool.h"
#include "history.h"
#include "jsonrpc.h"
#include <windows.h>
#include <stdbool.h>

#define MAX_MCP_CLIENTS 16
#define MAX_PENDING_TOOLS 32

typedef struct {
    Provider *provider;
    ToolRegistry *tools;
    History history;
    MCPClient *mcp_clients[MAX_MCP_CLIENTS];
    int mcp_count;
    bool running;
    // Streaming result tracking
    char *text_accum;    // Accumulated assistant text (reset before each stream)
    size_t text_len;
    // Pending tool calls collected during streaming (like Go's toolCalls slice)
    char *pending_tool_names[MAX_PENDING_TOOLS];
    char *pending_tool_ids[MAX_PENDING_TOOLS];
    char *pending_tool_inputs[MAX_PENDING_TOOLS];
    int pending_tool_count;
    // Ctrl+C / cancellation context (like miniclaude's context.Context)
    volatile long cancelled;       // Set to 1 when Ctrl+C pressed
    volatile long interrupt_count; // Number of rapid interrupts
    DWORD last_interrupt_time;     // Timestamp of last Ctrl+C
} REPL;

// Create/destroy REPL
REPL *repl_new(Provider *provider, ToolRegistry *tools);
void repl_free(REPL *repl);

// Pending tool call management
void repl_clear_tools(REPL *repl);
void repl_add_tool(REPL *repl, const char *name, const char *id, const char *input);

// Cancellation context (like miniclaude's context.WithCancel)
void repl_cancel(REPL *repl);          // Signal cancellation (like cancel())
void repl_reset_cancel(REPL *repl);    // Reset for next operation (like context reuse)
bool repl_is_cancelled(REPL *repl);    // Check if cancelled (like ctx.Done())

// Read line with Ctrl+C interrupt support (like miniclaude's readLineInterruptible)
// Returns strdup'd line, or NULL on EOF/interrupt
char *repl_read_line_interruptible(REPL *repl);

// Ctrl+C handler callback (installed by SetConsoleCtrlHandler)
// This is called from a signal context and must be async-signal-safe.
void repl_console_ctrl_handler(REPL *repl);

// Add MCP client
bool repl_add_mcp(REPL *repl, const char *name, const char *command);

// Run the REPL
int repl_run(REPL *repl);

// Utility
char *repl_read_line(const char *prompt);
void repl_print(const char *str);
void repl_print_error(const char *str);

#endif // REPL_H
