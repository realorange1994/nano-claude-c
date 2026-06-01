#include "provider.h"
#include "tool.h"
#include "http.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct Provider {
    ProviderType type;
    char *api_key;
    char *model;
    char *base_url;
    int max_tokens;
};

Provider *provider_new(ProviderType type, const char *api_key, const char *model, const char *base_url) {
    Provider *p = calloc(1, sizeof(Provider));
    if (!p) return NULL;
    p->type = type;
    p->api_key = api_key ? strdup(api_key) : NULL;
    p->model = model ? strdup(model) : NULL;
    p->base_url = base_url ? strdup(base_url) : NULL;
    if (!p->api_key || !p->model) {
        free(p->api_key); free(p->model); free(p->base_url); free(p);
        return NULL;
    }
    p->max_tokens = 4096;
    return p;
}

void provider_free(Provider *p) {
    if (!p) return;
    free(p->api_key);
    free(p->model);
    free(p->base_url);
    free(p);
}

const char *provider_model(Provider *p) {
    return p->model;
}

ProviderType provider_type(Provider *p) {
    return p->type;
}

char *provider_chat_sync(Provider *p, cJSON *messages) {
    if (!p || !messages) return NULL;
    
    char *json_body = NULL;
    char headers[1024];
    char url[512];
    
    if (p->type == PROVIDER_ANTHROPIC) {
        // Anthropic format
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(p->max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(false));
        
        json_body = cJSON_Print(body);
        cJSON_Delete(body);
        
        snprintf(headers, sizeof(headers),
            "Content-Type: application/json\n"
            "x-api-key: %s\n"
            "anthropic-version: 2023-06-01\n",
            p->api_key);
        
        if (p->base_url) {
            snprintf(url, sizeof(url), "%s/v1/messages", p->base_url);
        } else {
            snprintf(url, sizeof(url), "https://api.anthropic.com/v1/messages");
        }
    } else {
        // OpenAI format
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(p->max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(false));
        
        json_body = cJSON_Print(body);
        cJSON_Delete(body);
        
        snprintf(headers, sizeof(headers),
            "Content-Type: application/json\n"
            "Authorization: Bearer %s\n",
            p->api_key);
        
        if (p->base_url) {
            snprintf(url, sizeof(url), "%s/v1/chat/completions", p->base_url);
        } else {
            snprintf(url, sizeof(url), "https://api.openai.com/v1/chat/completions");
        }
    }
    
    if (!json_body) return NULL;

    char *response = http_post(url, headers, json_body, 120000);
    free(json_body);

    if (!response) return NULL;

    cJSON *json = cJSON_Parse(response);
    free(response);

    if (!json) {
        return NULL;
    }

    char *content = NULL;

    if (p->type == PROVIDER_ANTHROPIC) {
        cJSON *content_arr = cJSON_GetObjectItem(json, "content");
        if (content_arr && cJSON_GetArraySize(content_arr) > 0) {
            cJSON *item = cJSON_GetArrayItem(content_arr, 0);
            if (item) {
                cJSON *text = cJSON_GetObjectItem(item, "text");
                if (text && text->valuestring) {
                    content = strdup(text->valuestring);
                }
            }
        }
    } else {
        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        if (choices && cJSON_GetArraySize(choices) > 0) {
            cJSON *choice = cJSON_GetArrayItem(choices, 0);
            if (choice) {
                cJSON *msg = cJSON_GetObjectItem(choice, "message");
                if (msg) {
                    cJSON *msg_content = cJSON_GetObjectItem(msg, "content");
                    if (msg_content && msg_content->valuestring) {
                        content = strdup(msg_content->valuestring);
                    }
                }
            }
        }
    }

    cJSON_Delete(json);
    return content;
}

typedef struct {
    ChunkCallback callback;
    void *userdata;
    // Tool call state
    char current_tool_name[256];
    char current_tool_id[256];
    char *tool_input_buf;      // Dynamically allocated tool input buffer
    size_t tool_input_len;     // Current length of tool input
    size_t tool_input_cap;     // Capacity of tool input buffer
    int in_tool_use;
} StreamContext;

static void stream_context_free(StreamContext *ctx) {
    free(ctx->tool_input_buf);
}

static void stream_context_append_tool_input(StreamContext *ctx, const char *data, size_t data_len) {
    size_t needed = ctx->tool_input_len + data_len + 1;
    if (needed > ctx->tool_input_cap) {
        ctx->tool_input_cap = needed * 2;
        char *new_buf = realloc(ctx->tool_input_buf, ctx->tool_input_cap);
        if (!new_buf) {
            ctx->tool_input_cap = needed;
            return;
        }
        ctx->tool_input_buf = new_buf;
    }
    if (ctx->tool_input_buf && ctx->tool_input_cap > ctx->tool_input_len + data_len) {
        memcpy(ctx->tool_input_buf + ctx->tool_input_len, data, data_len);
        ctx->tool_input_len += data_len;
        ctx->tool_input_buf[ctx->tool_input_len] = '\0';
    }
}

static void stream_flush_tool(StreamContext *ctx) {
    if (!ctx->in_tool_use) return;

    // Parse JSON tool input with recovery for incomplete/partial JSON
    // (like miniclaude's parseAnthropicJSONArgs)
    char *input_json = NULL;
    if (ctx->tool_input_buf && ctx->tool_input_len > 0) {
        // First try direct parse
        cJSON *parsed = cJSON_Parse(ctx->tool_input_buf);
        if (parsed) {
            input_json = cJSON_PrintUnformatted(parsed);
            cJSON_Delete(parsed);
        } else {
            // JSON parse failed - try to extract a valid JSON object
            // by scanning for balanced braces (like extractJSONObject)
            const char *s = ctx->tool_input_buf;
            size_t s_len = ctx->tool_input_len;

            // Find first '{' that starts a complete JSON object
            for (size_t start = 0; start < s_len; start++) {
                if (s[start] != '{') continue;

                int depth = 0;
                int in_string = 0;
                int escaped = 0;

                for (size_t i = start; i < s_len; i++) {
                    unsigned char c = (unsigned char)s[i];

                    if (escaped) {
                        escaped = 0;
                        continue;
                    }
                    if (c == '\\' && in_string) {
                        escaped = 1;
                        continue;
                    }
                    if (c == '"') {
                        in_string = !in_string;
                        continue;
                    }
                    if (in_string) continue;

                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) {
                            // Found complete JSON object from start to i
                            size_t json_len = i - start + 1;
                            char *json_str = malloc(json_len + 1);
                            memcpy(json_str, s + start, json_len);
                            json_str[json_len] = '\0';

                            parsed = cJSON_Parse(json_str);
                            if (parsed) {
                                input_json = cJSON_PrintUnformatted(parsed);
                                cJSON_Delete(parsed);
                            }
                            free(json_str);
                            goto done_json_recovery;
                        }
                    }
                }
            }

done_json_recovery:
            ;
        }
    }
    if (!input_json) {
        input_json = strdup("{}");
    }

    StreamChunk chunk = {0};
    chunk.type = CHUNK_TOOL_CALL;
    chunk.tool_name = ctx->current_tool_name;
    chunk.tool_id = ctx->current_tool_id;
    chunk.tool_input = input_json;
    ctx->callback(&chunk, ctx->userdata);
    free(input_json);

    // Clear tool state
    ctx->current_tool_name[0] = '\0';
    ctx->current_tool_id[0] = '\0';
    ctx->tool_input_len = 0;
    if (ctx->tool_input_buf) ctx->tool_input_buf[0] = '\0';
    ctx->in_tool_use = 0;
}

static void stream_handle_text_delta(cJSON *delta, StreamContext *ctx) {
    cJSON *text = cJSON_GetObjectItem(delta, "text");
    if (text && text->valuestring) {
        StreamChunk chunk = {0};
        chunk.type = CHUNK_CONTENT;
        chunk.content = strdup(text->valuestring);
        ctx->callback(&chunk, ctx->userdata);
        free(chunk.content);
    }
}

static void stream_handle_content_block_start(cJSON *block, StreamContext *ctx) {
    cJSON *type = cJSON_GetObjectItem(block, "type");
    if (!type || !type->valuestring) return;

    if (strcmp(type->valuestring, "tool_use") == 0) {
        // Start of a tool call - capture name and id only
        cJSON *name = cJSON_GetObjectItem(block, "name");
        cJSON *id = cJSON_GetObjectItem(block, "id");

        if (name && name->valuestring) {
            strncpy(ctx->current_tool_name, name->valuestring, sizeof(ctx->current_tool_name) - 1);
        }
        if (id && id->valuestring) {
            strncpy(ctx->current_tool_id, id->valuestring, sizeof(ctx->current_tool_id) - 1);
        }

        // Clear tool input buffer - only collect from input_json_delta deltas
        // (like miniclaude's pendingToolArgs = nil pattern)
        ctx->tool_input_len = 0;
        if (ctx->tool_input_buf) ctx->tool_input_buf[0] = '\0';
        ctx->in_tool_use = 1;
    }
}

static void stream_handle_content_block_delta(cJSON *delta, StreamContext *ctx) {
    cJSON *type = cJSON_GetObjectItem(delta, "type");
    if (!type || !type->valuestring) return;

    if (strcmp(type->valuestring, "text_delta") == 0) {
        stream_handle_text_delta(delta, ctx);
    } else if (strcmp(type->valuestring, "input_json_delta") == 0) {
        // Tool input delta
        if (ctx->in_tool_use) {
            cJSON *partial = cJSON_GetObjectItem(delta, "partial_json");
            if (partial && partial->valuestring) {
                size_t len = strlen(partial->valuestring);
                stream_context_append_tool_input(ctx, partial->valuestring, len);
            }
        }
    }
}

static void stream_handle_content_block_end(cJSON *block, StreamContext *ctx) {
    cJSON *type = cJSON_GetObjectItem(block, "type");
    if (!type || !type->valuestring) return;
    
    if (strcmp(type->valuestring, "tool_use") == 0) {
        // End of tool call - flush it
        stream_flush_tool(ctx);
    }
}

static void stream_callback(const char *line, void *userdata) {
    StreamContext *ctx = (StreamContext *)userdata;
    
    // Skip empty lines
    if (!line || *line == '\0') return;
    
    // Handle [DONE] marker
    if (strcmp(line, "[DONE]") == 0) {
        // Flush any pending tool call
        stream_flush_tool(ctx);
        StreamChunk chunk = {0};
        chunk.type = CHUNK_DONE;
        ctx->callback(&chunk, ctx->userdata);
        return;
    }
    
    cJSON *json = cJSON_Parse(line);
    if (!json) {
        return;
    }
    
    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type || !type->valuestring) {
        cJSON_Delete(json);
        return;
    }
    
    if (strcmp(type->valuestring, "content_block_start") == 0) {
        cJSON *content_block = cJSON_GetObjectItem(json, "content_block");
        if (content_block) {
            stream_handle_content_block_start(content_block, ctx);
        }
    } else if (strcmp(type->valuestring, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (delta) {
            stream_handle_content_block_delta(delta, ctx);
        }
    } else if (strcmp(type->valuestring, "content_block_end") == 0) {
        cJSON *content_block = cJSON_GetObjectItem(json, "content_block");
        if (content_block) {
            stream_handle_content_block_end(content_block, ctx);
        }
    } else if (strcmp(type->valuestring, "message_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (delta) {
            cJSON *stop_reason = cJSON_GetObjectItem(delta, "stop_reason");
            if (stop_reason && stop_reason->valuestring) {
                // Flush any pending tool call
                stream_flush_tool(ctx);
                StreamChunk chunk = {0};
                chunk.type = CHUNK_DONE;
                chunk.stop_reason = strdup(stop_reason->valuestring);
                ctx->callback(&chunk, ctx->userdata);
                free(chunk.stop_reason);
            }
        }
    }
    
    cJSON_Delete(json);
}

static cJSON *build_anthropic_tool(Tool *tool) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddItemToObject(t, "name", cJSON_CreateString(tool->name));
    cJSON_AddItemToObject(t, "description", cJSON_CreateString(tool->description));
    if (tool->input_schema) {
        cJSON_AddItemToObject(t, "input_schema", cJSON_Duplicate(tool->input_schema, 1));
    } else {
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddItemToObject(schema, "type", cJSON_CreateString("object"));
        cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
        cJSON_AddItemToObject(t, "input_schema", schema);
    }
    return t;
}

bool provider_chat_stream(Provider *p, cJSON *messages, ChunkCallback callback, void *userdata, ToolRegistry *tools, const volatile long *cancelled) {
    if (!p || !messages || !callback) return false;

    char *json_body = NULL;
    char headers[1024];
    char url[512];

    if (p->type == PROVIDER_ANTHROPIC) {
        // Anthropic format
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(p->max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(true));

        // Add tools from registry so the model knows what tools are available
        if (tools && tools->count > 0) {
            cJSON *tools_arr = cJSON_CreateArray();
            for (int i = 0; i < tools->count; i++) {
                Tool *t = tools->tools[i];
                cJSON *tool_def = build_anthropic_tool(tools->tools[i]);
                if (tool_def) cJSON_AddItemToArray(tools_arr, tool_def);
            }
            cJSON_AddItemToObject(body, "tools", tools_arr);
        }

        json_body = cJSON_Print(body);
        cJSON_Delete(body);

        snprintf(headers, sizeof(headers),
            "Content-Type: application/json\n"
            "x-api-key: %s\n"
            "anthropic-version: 2023-06-01\n",
            p->api_key);

        if (p->base_url) {
            snprintf(url, sizeof(url), "%s/v1/messages", p->base_url);
        } else {
            snprintf(url, sizeof(url), "https://api.anthropic.com/v1/messages");
        }
    } else {
        // OpenAI format
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(p->max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(true));
        
        json_body = cJSON_Print(body);
        cJSON_Delete(body);
        
        snprintf(headers, sizeof(headers),
            "Content-Type: application/json\n"
            "Authorization: Bearer %s\n",
            p->api_key);
        
        if (p->base_url) {
            snprintf(url, sizeof(url), "%s/v1/chat/completions", p->base_url);
        } else {
            snprintf(url, sizeof(url), "https://api.openai.com/v1/chat/completions");
        }
    }
    
    if (!json_body) return false;
    
    StreamContext ctx;
    ctx.callback = callback;
    ctx.userdata = userdata;
    ctx.current_tool_name[0] = '\0';
    ctx.current_tool_id[0] = '\0';
    ctx.tool_input_buf = NULL;
    ctx.tool_input_len = 0;
    ctx.tool_input_cap = 0;
    ctx.in_tool_use = 0;

    bool success = http_post_stream(url, headers, json_body, stream_callback, &ctx, 120000, cancelled);

    stream_context_free(&ctx);
    free(json_body);
    return success;
}
