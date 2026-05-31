#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include "cJSON.h"

#define MAX_MESSAGES 1024

typedef enum {
    MSG_USER,
    MSG_ASSISTANT,
    MSG_TOOL
} MessageType;

typedef struct {
    MessageType type;
    char *role;
    char *content;
    char *tool_name;
    char *tool_id;
    char *tool_result;
} Message;

typedef struct {
    Message msgs[MAX_MESSAGES];
    int count;
    int total_tokens;
    char *summary;
    int reserve_tokens;
    int keep_recent_tokens;
} History;

// History management
void history_init(History *h, int reserve_tokens, int keep_recent_tokens);
void history_free(History *h);

// Add messages
void history_add(History *h, MessageType type, const char *content);

// Add a tool call to history (assistant side with tool_use block)
void history_add_tool_call(History *h, const char *tool_id, const char *tool_name, const char *tool_input_json);

// Add tool result (user side with tool_result block)
void history_add_tool(History *h, const char *tool_id, const char *tool_name, const char *tool_result);

// Get messages for API
cJSON *history_to_messages(History *h);

// Check if compaction is needed
bool history_needs_compact(History *h);

// Compact history (when context window is full)
void history_compact(History *h, char *(*summarize_fn)(const char*));

// Get token count estimate
int history_estimate_tokens(History *h);

#endif // HISTORY_H
