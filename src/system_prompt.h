#ifndef SYSTEM_PROMPT_H
#define SYSTEM_PROMPT_H

#include "tool.h"

// Boundary separating static (globally cacheable) from dynamic (per-session) content
#define SYSTEM_PROMPT_STATIC_BOUNDARY "<!-- STATIC_PROMPT_END -->"

// Build the full system prompt from tool registry and runtime info
// Returns a malloc'd string; caller must free it
char *system_prompt_build(ToolRegistry *tools, const char *model,
                          const char *permission_mode,
                          const char *project_instructions);
void system_prompt_free(char *prompt);

#endif // SYSTEM_PROMPT_H
