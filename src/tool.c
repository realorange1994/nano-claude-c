#include "tool.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>

// Tool registry
static int g_in_tool = 0;

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

char *tool_execute(ToolRegistry *reg, const char *name, cJSON *input, char **error) {
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
    char *result = tool->func(args, error);
    g_in_tool = 0;
    
    if (created_args) cJSON_Delete(args);
    
    return result;
}

void tool_register_builtins(ToolRegistry *reg) {
    // Read tool
    cJSON *read_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(read_schema, "type", cJSON_CreateString("object"));
    cJSON *read_props = cJSON_CreateObject();
    cJSON *path_prop = cJSON_CreateObject(); cJSON_AddItemToObject(path_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(path_prop, "description", cJSON_CreateString("File path to read")); cJSON_AddItemToObject(read_props, "path", path_prop);
    cJSON *offset_prop = cJSON_CreateObject(); cJSON_AddItemToObject(offset_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(offset_prop, "description", cJSON_CreateString("Byte offset to start reading")); cJSON_AddItemToObject(read_props, "offset", offset_prop);
    cJSON *limit_prop = cJSON_CreateObject(); cJSON_AddItemToObject(limit_prop, "type", cJSON_CreateString("integer")); cJSON_AddItemToObject(limit_prop, "description", cJSON_CreateString("Number of bytes to read")); cJSON_AddItemToObject(read_props, "limit", limit_prop);
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
    tool_register(reg, "Write", "Write content to file", write_schema, tool_write_file);

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
    cJSON *pat_prop = cJSON_CreateObject(); cJSON_AddItemToObject(pat_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(pat_prop, "description", cJSON_CreateString("Search pattern")); cJSON_AddItemToObject(grep_props, "pattern", pat_prop);
    cJSON *gpath_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gpath_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gpath_prop, "description", cJSON_CreateString("File path to search")); cJSON_AddItemToObject(grep_props, "path", gpath_prop);
    cJSON_AddItemToObject(grep_schema, "properties", grep_props);
    cJSON *grep_required = cJSON_CreateArray(); cJSON_AddItemToArray(grep_required, cJSON_CreateString("pattern")); cJSON_AddItemToArray(grep_required, cJSON_CreateString("path")); cJSON_AddItemToObject(grep_schema, "required", grep_required);
    tool_register(reg, "Grep", "Search for pattern in file", grep_schema, tool_grep);

    // Glob tool
    cJSON *glob_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(glob_schema, "type", cJSON_CreateString("object"));
    cJSON *glob_props = cJSON_CreateObject();
    cJSON *gpat_prop = cJSON_CreateObject(); cJSON_AddItemToObject(gpat_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(gpat_prop, "description", cJSON_CreateString("Glob pattern")); cJSON_AddItemToObject(glob_props, "pattern", gpat_prop);
    cJSON_AddItemToObject(glob_schema, "properties", glob_props);
    cJSON *glob_required = cJSON_CreateArray(); cJSON_AddItemToArray(glob_required, cJSON_CreateString("pattern")); cJSON_AddItemToObject(glob_schema, "required", glob_required);
    tool_register(reg, "Glob", "Find files matching pattern", glob_schema, tool_glob);

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
    cJSON_AddItemToObject(timeout_prop, "type", cJSON_CreateNumber(120)); 
    cJSON_AddItemToObject(timeout_prop, "description", cJSON_CreateString("Timeout in seconds (default: 120, max: 600)")); 
    cJSON_AddItemToObject(shell_props, "timeout", timeout_prop);
    cJSON_AddItemToObject(shell_schema, "properties", shell_props);
    cJSON *shell_required = cJSON_CreateArray(); 
    cJSON_AddItemToArray(shell_required, cJSON_CreateString("command")); 
    cJSON_AddItemToObject(shell_schema, "required", shell_required);
    tool_register(reg, "Shell", 
        "Execute a shell command. Use for package installs, builds, git operations, and any shell task. "
        "SAFETY: Dangerous patterns (rm -rf /, fork bombs, git reset --hard) are blocked. "
        "IMPORTANT: Commands requiring user input (y/n prompts, password) will be killed with stall detection.",
        shell_schema, tool_exec);

    // Calc tool
    cJSON *calc_schema = cJSON_CreateObject();
    cJSON_AddItemToObject(calc_schema, "type", cJSON_CreateString("object"));
    cJSON *calc_props = cJSON_CreateObject();
    cJSON *expr_prop = cJSON_CreateObject(); cJSON_AddItemToObject(expr_prop, "type", cJSON_CreateString("string")); cJSON_AddItemToObject(expr_prop, "description", cJSON_CreateString("Math expression")); cJSON_AddItemToObject(calc_props, "expression", expr_prop);
    cJSON_AddItemToObject(calc_schema, "properties", calc_props);
    cJSON *calc_required = cJSON_CreateArray(); cJSON_AddItemToArray(calc_required, cJSON_CreateString("expression")); cJSON_AddItemToObject(calc_schema, "required", calc_required);
    tool_register(reg, "Calc", "Calculate expression", calc_schema, tool_calc);
}
