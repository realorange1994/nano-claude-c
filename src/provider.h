#ifndef PROVIDER_H
#define PROVIDER_H

#include <stdbool.h>
#include "cJSON.h"

typedef enum {
    PROVIDER_ANTHROPIC,
    PROVIDER_OPENAI
} ProviderType;

typedef struct Provider Provider;

// Stream chunk types
typedef enum {
    CHUNK_CONTENT,
    CHUNK_TOOL_CALL,
    CHUNK_DONE
} ChunkType;

typedef struct {
    ChunkType type;
    char *content;
    char *tool_name;
    char *tool_id;
    char *tool_input;  // JSON string
    char *stop_reason;
} StreamChunk;

// Provider lifecycle
Provider *provider_new(ProviderType type, const char *api_key, const char *model, const char *base_url, int max_tokens);
void provider_free(Provider *p);

typedef struct ToolRegistry ToolRegistry;

// Streaming chat
typedef void (*ChunkCallback)(const StreamChunk *chunk, void *userdata);
// cancelled: if non-NULL, checked during streaming for Ctrl+C cancellation
bool provider_chat_stream(Provider *p, cJSON *messages, ChunkCallback callback, void *userdata, ToolRegistry *tools, const volatile long *cancelled);

// Synchronous chat (for compact summary)
char *provider_chat_sync(Provider *p, cJSON *messages);

void provider_set_max_tokens(Provider *p, int max_tokens);
const char *provider_model(Provider *p);
ProviderType provider_type(Provider *p);

#endif // PROVIDER_H
