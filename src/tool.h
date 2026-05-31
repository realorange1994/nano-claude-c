#ifndef TOOL_H
#define TOOL_H

#include <stdbool.h>
#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Tool timeout flag (checked by long-running operations)
#ifdef _WIN32
extern volatile LONG g_tool_timeout;
#else
extern volatile int g_tool_timeout;
#endif

#define MAX_TOOLS 128

typedef struct Tool Tool;
typedef struct ToolRegistry ToolRegistry;

// Tool function signature
typedef char* (*ToolFunc)(cJSON *input, char **error);

// Tool definition
struct Tool {
    const char *name;
    const char *description;
    cJSON *input_schema;  // JSON Schema for the tool
    ToolFunc func;
};

// Tool registry
struct ToolRegistry {
    Tool *tools[MAX_TOOLS];
    int count;
};

// Registry management
ToolRegistry *tool_registry_new(void);
void tool_registry_free(ToolRegistry *reg);

// Register a tool
bool tool_register(ToolRegistry *reg, const char *name, const char *description,
                   cJSON *input_schema, ToolFunc func);

// Find a tool
Tool *tool_find(ToolRegistry *reg, const char *name);

// Execute a tool safely (with panic recovery)
char *tool_execute(ToolRegistry *reg, const char *name, cJSON *input, char **error);

// Built-in tool implementations
char *tool_read_file(cJSON *input, char **error);
char *tool_write_file(cJSON *input, char **error);
char *tool_edit_file(cJSON *input, char **error);
char *tool_grep(cJSON *input, char **error);
char *tool_glob(cJSON *input, char **error);
char *tool_exec(cJSON *input, char **error);
char *tool_calc(cJSON *input, char **error);

// Register all built-in tools
void tool_register_builtins(ToolRegistry *reg);

#endif // TOOL_H
