#include "tool.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include <signal.h>

// Tool registry
static int g_in_tool = 0;

// Tool timeout flag (set to 1 to request cancellation)
volatile LONG g_tool_timeout = 0;
    g_interrupted = 0;

// Interruption flag (set by Ctrl+C handler)
volatile LONG g_interrupted = 0;

// Exception handling with signals (no setjmp)
static volatile int g_crashed = 0;

// Store old handlers
typedef void (*sig_handler_t)(int);
static sig_handler_t g_old_sigabrt = NULL;
static sig_handler_t g_old_sigfpe = NULL;
static sig_handler_t g_old_sigsegv = NULL;
static sig_handler_t g_old_sigill = NULL;
static sig_handler_t g_old_sigint = NULL;

// Simple file-based debug logging
static void debug_log_disabled(const char *msg) {
    FILE *f = fopen("E:\\\\Git\\\\nanoclaude-c\\\\debug.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

// Crash handler
static void tool_crash_handler(int sig) {
    g_crashed = 1;
    FILE *f = fopen("E:\\\\Git\\\\nanoclaude-c\\\\debug.log", "a");
    if (f) {
        fprintf(f, "[CRASH] Signal %d received\n", sig);
        fclose(f);
    }
    // Don't try to longjmp, just set flag and return
}

// Begin crash protection - install signal handlers
static void _begin_crash_protection(void) {
    g_old_sigabrt = signal(SIGABRT, tool_crash_handler);
    g_old_sigfpe = signal(SIGFPE, tool_crash_handler);
    g_old_sigsegv = signal(SIGSEGV, tool_crash_handler);
    g_old_sigill = signal(SIGILL, tool_crash_handler);
    g_old_sigint = signal(SIGINT, tool_crash_handler);
    g_crashed = 0;
}

// End crash protection - restore original handlers
static void _end_crash_protection(void) {
    if (g_old_sigabrt && g_old_sigabrt != SIG_ERR) signal(SIGABRT, g_old_sigabrt);
    if (g_old_sigfpe && g_old_sigfpe != SIG_ERR) signal(SIGFPE, g_old_sigfpe);
    if (g_old_sigsegv && g_old_sigsegv != SIG_ERR) signal(SIGSEGV, g_old_sigsegv);
    if (g_old_sigill && g_old_sigill != SIG_ERR) signal(SIGILL, g_old_sigill);
    if (g_old_sigint && g_old_sigint != SIG_ERR) signal(SIGINT, g_old_sigint);
}

ToolRegistry *tool_registry_new(void) {
    ToolRegistry *reg = calloc(1, sizeof(ToolRegistry));
    return reg;
}

void tool_registry_free(ToolRegistry *reg) {
    if (!reg) return;
    for (int i = 0; i < reg->count; i++) {
        Tool *t = reg->tools[i];
        free((void*)t->name);
        free((void*)t->description);
        cJSON_Delete(t->input_schema);
        free(t);
    }
    free(reg);
}

bool tool_register(ToolRegistry *reg, const char *name, const char *description,
                   cJSON *input_schema, ToolFunc func) {
    if (reg->count >= MAX_TOOLS) return false;
    Tool *t = calloc(1, sizeof(Tool));
    if (!t) return false;
    
    t->name = name ? strdup(name) : NULL;
    t->description = description ? strdup(description) : NULL;
    t->input_schema = input_schema;
    t->func = func;
    reg->tools[reg->count++] = t;
    return true;
}

Tool *tool_find(ToolRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0) {
            return reg->tools[i];
        }
    }
    return NULL;
}

// Tool timeout (30 seconds)
#define TOOL_TIMEOUT_MS 30000

// Get current tick count (Windows)
static DWORD tool_get_tick_count(void) {
    return GetTickCount();
}

char *tool_execute(ToolRegistry *reg, const char *name, cJSON *input, char **error) {
    char dbg[256];
    snprintf(dbg, sizeof(dbg), "[DEBUG] tool_execute: starting '%s'", name ? name : "(null)");
    /* debug_log disabled */;
    
    if (!reg || !name) {
        if (error) *error = strdup("invalid arguments");
        return NULL;
    }
    
    Tool *tool = tool_find(reg, name);
    if (!tool) {
        if (error) *error = strdup("tool not found");
        return NULL;
    }
    
    if (g_in_tool) {
        if (error) *error = strdup("recursive tool call detected");
        return NULL;
    }
    
    // Create empty object if no input provided
    cJSON *args = input;
    bool created_args = false;
    if (!args) {
        args = cJSON_CreateObject();
        created_args = true;
    }
    
    g_in_tool = 1;
    
    // Set up timeout
    DWORD start_tick = tool_get_tick_count();
    g_tool_timeout = 0;
    g_interrupted = 0;
    
    // Install crash handlers
    _begin_crash_protection();
    
    snprintf(dbg, sizeof(dbg), "[DEBUG] tool_execute: calling '%s'", name);
    /* debug_log disabled */;
    
    // Check for interruption before executing
    if (g_interrupted) {
        if (error) *error = strdup("interrupted");
        result = NULL;
        goto cleanup;
    }

    // Execute tool
    char *result = tool->func(args, error);
    
    snprintf(dbg, sizeof(dbg), "[DEBUG] tool_execute: '%s' returned", name);
    /* debug_log disabled */;
    
    // Check if tool crashed
    if (g_crashed) {
        snprintf(dbg, sizeof(dbg), "[DEBUG] tool_execute: '%s' CRASHED!", name);
        /* debug_log disabled */;
        if (result) { free(result); result = NULL; }
        if (error && !*error) {
            *error = strdup("tool crashed and was recovered");
        }
    }
    

    
    snprintf(dbg, sizeof(dbg), "[DEBUG] tool_execute: '%s' done", name);
    /* debug_log disabled */;
    
cleanup:
    // Cleanup
    _end_crash_protection();
    g_in_tool = 0;
    g_tool_timeout = 0;
    g_interrupted = 0;

    if (created_args) cJSON_Delete(args);

    // Check timeout
    DWORD elapsed = tool_get_tick_count() - start_tick;
    if (elapsed > TOOL_TIMEOUT_MS) {
        g_tool_timeout = 1;
        if (result) { free(result); result = NULL; }
        if (error && !*error) {
            *error = strdup("tool execution timeout (>30s)");
        }
    }
    
    return result;
}

void tool_register_builtins(ToolRegistry *reg) {
    // Read tool
    cJSON *read_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(read_schema, "type", cJSON_CreateString("object"));
    cJSON *read_props = cJSON_CreateObject();
    cJSON *path_prop = cJSON_CreateObject(); cJSON_AddItemToObject(path_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(path_prop, "description", cJSON_CreateString("File path to read")); cJSON_AddItemToObject(read_props, "path", path_prop);
    cJSON *offset_prop = cJSON_CreateObject(); cJSON_AddItemToObject(offset_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(offset_prop, "description", cJSON_CreateString("Line number to start reading from (1-indexed, default: 1)")); cJSON_AddItemToObject(read_props, "offset", offset_prop);
    cJSON *limit_prop = cJSON_CreateObject(); cJSON_AddItemToObject(limit_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(limit_prop, "description", cJSON_CreateString("Maximum number of lines to read (default: 500). Always use at least 250 to minimize tool calls.")); cJSON_AddItemToObject(read_props, "limit", limit_prop);
    cJSON_AddItemToObject(read_schema, "properties", read_props);
    cJSON *read_required = cJSON_CreateArray(); cJSON_AddItemToArray(read_required, cJSON_CreateString("path")); cJSON_AddItemToObject(read_schema, "required", read_required);
    tool_register(reg, "Read", "Read file contents", read_schema, tool_read_file);

    // Write tool
    cJSON *write_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(write_schema, "type", cJSON_CreateString("object"));
    cJSON *write_props = cJSON_CreateObject();
    cJSON *wpath_prop = cJSON_CreateObject(); cJSON_AddItemToObject(wpath_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(wpath_prop, "description", cJSON_CreateString("File path to write")); cJSON_AddItemToObject(write_props, "path", wpath_prop);
    cJSON *wcontent_prop = cJSON_CreateObject(); cJSON_AddItemToObject(wcontent_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(wcontent_prop, "description", cJSON_CreateString("Content to write")); cJSON_AddItemToObject(write_props, "content", wcontent_prop);
    cJSON_AddItemToObject(write_schema, "properties", write_props);
    cJSON *write_required = cJSON_CreateArray(); cJSON_AddItemToArray(write_required, cJSON_CreateString("path")); cJSON_AddItemToArray(write_required, cJSON_CreateString("content")); cJSON_AddItemToObject(write_schema, "required", write_required);
    tool_register(reg, "Write", "Write content to a file. Creates the file if it doesn't exist, overwrites if it does. Automatically creates parent directories.", write_schema, tool_write_file);

    // Edit tool
    cJSON *edit_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(edit_schema, "type", cJSON_CreateString("object"));
    cJSON *edit_props = cJSON_CreateObject();
    cJSON *epath_prop = cJSON_CreateObject(); cJSON_AddItemToObject(epath_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(epath_prop, "description", cJSON_CreateString("File path to edit")); cJSON_AddItemToObject(edit_props, "path", epath_prop);
    cJSON *old_prop = cJSON_CreateObject(); cJSON_AddItemToObject(old_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(old_prop, "description", cJSON_CreateString("Text to replace")); cJSON_AddItemToObject(edit_props, "old_string", old_prop);
    cJSON *new_prop = cJSON_CreateObject(); cJSON_AddItemToObject(new_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(new_prop, "description", cJSON_CreateString("Replacement text")); cJSON_AddItemToObject(edit_props, "new_string", new_prop);
    cJSON_AddItemToObject(edit_schema, "properties", edit_props);
    cJSON *edit_required = cJSON_CreateArray(); cJSON_AddItemToArray(edit_required, cJSON_CreateString("path")); cJSON_AddItemToArray(edit_required, cJSON_CreateString("old_string")); cJSON_AddItemToArray(edit_required, cJSON_CreateString("new_string")); cJSON_AddItemToObject(edit_schema, "required", edit_required);
    tool_register(reg, "Edit", "Edit file content", edit_schema, tool_edit_file);

    // Grep tool
    cJSON *grep_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(grep_schema, "type", cJSON_CreateString("object"));
    cJSON *grep_props = cJSON_CreateObject();
    cJSON *pat_prop = cJSON_CreateObject(); cJSON_AddItemToObject(pat_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(pat_prop, "description", cJSON_CreateString("REQUIRED: The regex pattern to search for")); cJSON_AddItemToObject(grep_props, "pattern", pat_prop);
    cJSON *gpath_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gpath_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gpath_prop, "description", cJSON_CreateString("Path to search in (default: current directory)")); cJSON_AddItemToObject(grep_props, "path", gpath_prop);
    cJSON *glob_prop = cJSON_CreateObject(); cJSON_AddItemToObject(glob_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(glob_prop, "description", cJSON_CreateString("Glob pattern to filter files (e.g., '*.go', '**/*.txt')")); cJSON_AddItemToObject(grep_props, "glob", glob_prop);
    cJSON *ftype_prop = cJSON_CreateObject(); cJSON_AddItemToObject(ftype_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(ftype_prop, "description", cJSON_CreateString("File type filter (e.g., 'go', 'py', 'js', 'txt')")); cJSON_AddItemToObject(grep_props, "fileType", ftype_prop);
    cJSON *ctx_prop = cJSON_CreateObject(); cJSON_AddItemToObject(ctx_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(ctx_prop, "description", cJSON_CreateString("Lines of context before and after matches")); cJSON_AddItemToObject(grep_props, "context", ctx_prop);
    cJSON *mcnt_prop = cJSON_CreateObject(); cJSON_AddItemToObject(mcnt_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(mcnt_prop, "description", cJSON_CreateString("Maximum matches per file (default: 100, 0 = unlimited)")); cJSON_AddItemToObject(grep_props, "maxCount", mcnt_prop);
    cJSON *mres_prop = cJSON_CreateObject(); cJSON_AddItemToObject(mres_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(mres_prop, "description", cJSON_CreateString("Maximum total results (default: 250)")); cJSON_AddItemToObject(grep_props, "maxResults", mres_prop);
    cJSON *omode_prop = cJSON_CreateObject(); cJSON_AddItemToObject(omode_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(omode_prop, "description", cJSON_CreateString("Output mode: 'content' (default), 'files_with_matches', 'count'")); cJSON_AddItemToObject(grep_props, "outputMode", omode_prop);
    cJSON *ibinary_prop = cJSON_CreateObject(); cJSON_AddItemToObject(ibinary_prop, "type", cJSON_CreateString("boolean")); cJSON_AddItemToObject(ibinary_prop, "description", cJSON_CreateString("Include binary files in search (default: false)")); cJSON_AddItemToObject(grep_props, "includeBinary", ibinary_prop);
    cJSON *cs_prop = cJSON_CreateObject(); cJSON_AddItemToObject(cs_prop, "type", cJSON_CreateString("boolean")); cJSON_AddItemToObject(cs_prop, "description", cJSON_CreateString("Case sensitive matching (default: false)")); cJSON_AddItemToObject(grep_props, "caseSensitive", cs_prop);
    cJSON *mll_prop = cJSON_CreateObject(); cJSON_AddItemToObject(mll_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(mll_prop, "description", cJSON_CreateString("Maximum line length to include (default: 500)")); cJSON_AddItemToObject(grep_props, "maxLineLength", mll_prop);
    cJSON_AddItemToObject(grep_schema, "properties", grep_props);
    cJSON *grep_required = cJSON_CreateArray(); cJSON_AddItemToArray(grep_required, cJSON_CreateString("pattern")); cJSON_AddItemToObject(grep_schema, "required", grep_required);
    tool_register(reg, "Grep", "Search file contents for a pattern. Supports regex patterns, glob filters, file type filters, and context lines.", grep_schema, tool_grep);

    // Glob tool
    cJSON *glob_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(glob_schema, "type", cJSON_CreateString("object"));
    cJSON *glob_props = cJSON_CreateObject();
    cJSON *gpat_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gpat_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gpat_prop, "description", cJSON_CreateString("REQUIRED: Glob pattern to match files, e.g. '*.go', '**/*.json'")); cJSON_AddItemToObject(glob_props, "pattern", gpat_prop);
    cJSON *gglob_path_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gglob_path_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gglob_path_prop, "description", cJSON_CreateString("Directory to search in (default: .)")); cJSON_AddItemToObject(glob_props, "path", gglob_path_prop);
    cJSON *gtype_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gtype_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gtype_prop, "description", cJSON_CreateString("Type filter: 'file' (default), 'dir', 'all'")); cJSON_AddItemToObject(glob_props, "type", gtype_prop);
    cJSON *gres_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gres_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(gres_prop, "description", cJSON_CreateString("Maximum results to return (default: 100)")); cJSON_AddItemToObject(glob_props, "maxResults", gres_prop);
    cJSON_AddItemToObject(glob_schema, "properties", glob_props);
    cJSON *glob_required = cJSON_CreateArray(); cJSON_AddItemToArray(glob_required, cJSON_CreateString("pattern")); cJSON_AddItemToObject(glob_schema, "required", glob_required);
    tool_register(reg, "Glob", "Search for files by glob pattern. Supports recursive matching (**) and type filtering.", glob_schema, tool_glob);

    // Shell tool
    cJSON *shell_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(shell_schema, "type", cJSON_CreateString("object"));
    cJSON *shell_props = cJSON_CreateObject();
    cJSON *cmd_prop = cJSON_CreateObject(); 
    cJSON_AddItemToObject(cmd_prop, "type", cJSON_CreateString("string")); 
    cJSON_AddItemToObject(cmd_prop, "description", cJSON_CreateString("REQUIRED: Shell command to execute")); 
    cJSON_AddItemToObject(shell_props, "command", cmd_prop);
    cJSON *cwd_prop = cJSON_CreateObject(); 
    cJSON_AddItemToObject(cwd_prop, "type", cJSON_CreateString("string")); 
    cJSON_AddItemToObject(cwd_prop, "description", cJSON_CreateString("Working directory for the command")); 
    cJSON_AddItemToObject(shell_props, "cwd", cwd_prop);
    cJSON *timeout_prop = cJSON_CreateObject(); 
    cJSON_AddItemToObject(timeout_prop, "type", cJSON_CreateString("integer")); 
    cJSON_AddItemToObject(timeout_prop, "description", cJSON_CreateString("Timeout in seconds (default: 120, max: 600)")); 
    cJSON_AddItemToObject(shell_props, "timeout", timeout_prop);
    cJSON *env_prop = cJSON_CreateObject(); 
    cJSON_AddItemToObject(env_prop, "type", cJSON_CreateString("object")); 
    cJSON_AddItemToObject(env_prop, "description", cJSON_CreateString("Environment variables to set (e.g. env={\"GOOS\": \"linux\"})")); 
    cJSON_AddItemToObject(shell_props, "env", env_prop);
    cJSON_AddItemToObject(shell_schema, "properties", shell_props);
    cJSON *shell_required = cJSON_CreateArray(); 
    cJSON_AddItemToArray(shell_required, cJSON_CreateString("command")); 
    cJSON_AddItemToObject(shell_schema, "required", shell_required);
    tool_register(reg, "Shell", 
        "Execute a shell command. Use for package installs, builds, git operations, and any shell task. "
        "On Windows, uses Git Bash if available, otherwise PowerShell. "
        "SAFETY: Dangerous patterns (rm -rf /, fork bombs, git reset --hard, docker system prune) are blocked. "
        "Output is truncated to last 2000 lines or 50KB. "
        "Use the env parameter to set environment variables. "
        "IMPORTANT: stdin is disconnected �� commands requiring user input (password prompts, y/n) will be killed with stall detection. "
        "Use non-interactive flags instead (e.g., sudo -S, apt-get -y, echo y | command, --yes).",
        shell_schema, tool_exec);

    // Calc tool
    cJSON *calc_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(calc_schema, "type", cJSON_CreateString("object"));
    cJSON *calc_props = cJSON_CreateObject();
    cJSON *calc_expr_prop = cJSON_CreateObject(); cJSON_AddItemToObject(calc_expr_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(calc_expr_prop, "description", cJSON_CreateString("REQUIRED: The mathematical expression to evaluate")); cJSON_AddItemToObject(calc_props, "expr", calc_expr_prop);
    cJSON_AddItemToObject(calc_schema, "properties", calc_props);
    cJSON *calc_required = cJSON_CreateArray(); cJSON_AddItemToArray(calc_required, cJSON_CreateString("expr")); cJSON_AddItemToObject(calc_schema, "required", calc_required);
    tool_register(reg, "Calc", "Calculate a mathematical expression and return the result. Supports arithmetic (+, -, *, /, %), power (^), implicit multiplication (2(3), pi(3)), parentheses, variables (pi, e, phi, tau), and functions. Trigonometric functions use RADIANS (sin(pi/2)=1, cos(pi)=-1, sin(0)=0). Supported functions: sqrt, cbrt, pow, sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, log (base 2nd arg), ln, log2, log10, exp, abs, sign, floor, ceil, round, min, max, fact (factorial), deg (radians to degrees), rad (degrees to radians). Examples: 2+3*4, sin(pi/2), sqrt(16)+cos(pi), (1+2)^3, log(8,2), pi*2, deg(pi) (returns 180), rad(180) (returns pi)", calc_schema, tool_calc);
}
