#include "provider.h"
#include "tool.h"
#include "http.h"
#include "buffer.h"
#include "config.h"
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

Provider *provider_new(ProviderType type, const char *api_key, const char *model, const char *base_url, int max_tokens) {
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
    p->max_tokens = max_tokens > 0 ? max_tokens : 8192;
    return p;
}

void provider_free(Provider *p) {
    if (!p) return;
    free(p->api_key);
    free(p->model);
    free(p->base_url);
    free(p);
}

void provider_set_max_tokens(Provider *p, int max_tokens) {
    if (p && max_tokens > 0) p->max_tokens = max_tokens;
}

const char *provider_model(Provider *p) {
    return p->model;
}

ProviderType provider_type(Provider *p) {
    return p->type;
}

// ============================================================================
// Helper: prepend system prompt as system role message (for OpenAI format)
// ============================================================================

static cJSON *prepend_system_message(cJSON *messages, const char *system_prompt) {
    if (!system_prompt || !system_prompt[0]) return messages;
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddItemToObject(sys_msg, "role", cJSON_CreateString("system"));
    cJSON_AddItemToObject(sys_msg, "content", cJSON_CreateString(system_prompt));
    cJSON_InsertItemInArray(messages, 0, sys_msg);
    return messages;
}

// ============================================================================
// Helper: build OpenAI-style tool definitions
// ============================================================================

static cJSON *build_openai_tool(Tool *tool) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddItemToObject(t, "type", cJSON_CreateString("function"));
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddItemToObject(fn, "name", cJSON_CreateString(tool->name));
    cJSON_AddItemToObject(fn, "description", cJSON_CreateString(tool->description));
    if (tool->input_schema) {
        cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(tool->input_schema, 1));
    } else {
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddItemToObject(schema, "type", cJSON_CreateString("object"));
        cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
        cJSON_AddItemToObject(fn, "parameters", schema);
    }
    cJSON_AddItemToObject(t, "function", fn);
    return t;
}

// ============================================================================
// Helper: build Anthropic-style tool definitions
// ============================================================================

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

// ============================================================================
// Sync chat (for compaction summary)
// ============================================================================

char *provider_chat_sync(Provider *p, cJSON *messages, int max_tokens_override, const char *system_prompt) {
    if (!p || !messages) return NULL;

    int max_tokens = max_tokens_override > 0 ? max_tokens_override : p->max_tokens;

    char *json_body = NULL;
    char headers[1024];
    char url[512];

    if (p->type == PROVIDER_ANTHROPIC) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(false));
        if (system_prompt && system_prompt[0]) {
            cJSON_AddItemToObject(body, "system", cJSON_CreateString(system_prompt));
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
        cJSON *msgs = cJSON_Duplicate(messages, 1);
        if (system_prompt && system_prompt[0]) {
            msgs = prepend_system_message(msgs, system_prompt);
        }

        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", msgs);
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(max_tokens));

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

    if (!json) return NULL;

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

// ============================================================================
// Anthropic streaming parser
// ============================================================================

typedef struct {
    ChunkCallback callback;
    void *userdata;
    char current_tool_name[256];
    char current_tool_id[256];
    char *tool_input_buf;
    size_t tool_input_len;
    size_t tool_input_cap;
    int in_tool_use;
    int non_json_logged;
} StreamContext;

static void stream_context_free(StreamContext *ctx) {
    free(ctx->tool_input_buf);
}

static void stream_context_append_tool_input(StreamContext *ctx, const char *data, size_t data_len) {
    size_t needed = ctx->tool_input_len + data_len + 1;
    if (needed > ctx->tool_input_cap) {
        ctx->tool_input_cap = needed * 2;
        char *new_buf = realloc(ctx->tool_input_buf, ctx->tool_input_cap);
        if (!new_buf) { ctx->tool_input_cap = needed; return; }
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

    char *input_json = NULL;
    if (ctx->tool_input_buf && ctx->tool_input_len > 0) {
        cJSON *parsed = cJSON_Parse(ctx->tool_input_buf);
        if (parsed) {
            input_json = cJSON_PrintUnformatted(parsed);
            cJSON_Delete(parsed);
        } else {
            // JSON recovery: scan for balanced braces
            const char *s = ctx->tool_input_buf;
            size_t s_len = ctx->tool_input_len;
            for (size_t start = 0; start < s_len; start++) {
                if (s[start] != '{') continue;
                int depth = 0, in_string = 0, escaped = 0;
                for (size_t i = start; i < s_len; i++) {
                    unsigned char c = (unsigned char)s[i];
                    if (escaped) { escaped = 0; continue; }
                    if (c == '\\' && in_string) { escaped = 1; continue; }
                    if (c == '"') { in_string = !in_string; continue; }
                    if (in_string) continue;
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) {
                            size_t json_len = i - start + 1;
                            char *json_str = malloc(json_len + 1);
                            memcpy(json_str, s + start, json_len);
                            json_str[json_len] = '\0';
                            parsed = cJSON_Parse(json_str);
                            if (parsed) { input_json = cJSON_PrintUnformatted(parsed); cJSON_Delete(parsed); }
                            free(json_str);
                            goto done_json_recovery;
                        }
                    }
                }
            }
done_json_recovery: ;
        }
    }
    if (!input_json) input_json = strdup("{}");

    StreamChunk chunk = {0};
    chunk.type = CHUNK_TOOL_CALL;
    chunk.tool_name = ctx->current_tool_name;
    chunk.tool_id = ctx->current_tool_id;
    chunk.tool_input = input_json;
    ctx->callback(&chunk, ctx->userdata);
    free(input_json);

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
        cJSON *name = cJSON_GetObjectItem(block, "name");
        cJSON *id = cJSON_GetObjectItem(block, "id");
        if (name && name->valuestring) strncpy(ctx->current_tool_name, name->valuestring, sizeof(ctx->current_tool_name) - 1);
        if (id && id->valuestring) strncpy(ctx->current_tool_id, id->valuestring, sizeof(ctx->current_tool_id) - 1);
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
        stream_flush_tool(ctx);
    }
}

// Anthropic SSE stream callback
static void anthropic_stream_callback(const char *line, void *userdata) {
    StreamContext *ctx = (StreamContext *)userdata;
    if (!line || *line == '\0') return;

    if (strcmp(line, "[DONE]") == 0) {
        stream_flush_tool(ctx);
        StreamChunk chunk = {0};
        chunk.type = CHUNK_DONE;
        ctx->callback(&chunk, ctx->userdata);
        return;
    }

    cJSON *json = cJSON_Parse(line);
    if (!json) {
        if (!ctx->non_json_logged && strlen(line) > 10) {
            DEBUG_LOG("[DEBUG][anthropic_stream] Non-JSON: %.300s\n", line);
            ctx->non_json_logged = 1;
        }
        return;
    }

    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type || !type->valuestring) { cJSON_Delete(json); return; }

    if (strcmp(type->valuestring, "content_block_start") == 0) {
        cJSON *cb = cJSON_GetObjectItem(json, "content_block");
        if (cb) stream_handle_content_block_start(cb, ctx);
    } else if (strcmp(type->valuestring, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (delta) stream_handle_content_block_delta(delta, ctx);
    } else if (strcmp(type->valuestring, "content_block_end") == 0) {
        cJSON *cb = cJSON_GetObjectItem(json, "content_block");
        if (cb) stream_handle_content_block_end(cb, ctx);
    } else if (strcmp(type->valuestring, "message_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (delta) {
            cJSON *stop_reason = cJSON_GetObjectItem(delta, "stop_reason");
            if (stop_reason && stop_reason->valuestring) {
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

// ============================================================================
// OpenAI streaming parser
// OpenAI SSE format:
//   data: {"id":"...","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]}
//   data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_x","function":{"name":"Read","arguments":"{\"file_path\":\"..."}}}]}}]}
//   data: [DONE]
// ============================================================================

typedef struct {
    ChunkCallback callback;
    void *userdata;
    // OpenAI tool calls use index-based incremental arguments
    char tool_names[32][256];    // tool call names, indexed by OpenAI's "index"
    char tool_ids[32][256];      // tool call IDs, indexed by "index"
    char *tool_args[32];         // accumulated argument strings
    size_t tool_args_len[32];
    size_t tool_args_cap[32];
    int max_tool_index;          // highest index seen so far
    int done_sent;               // avoid sending DONE twice
    int non_json_logged;
} OpenAIStreamContext;

static void openai_stream_context_free(OpenAIStreamContext *ctx) {
    for (int i = 0; i <= ctx->max_tool_index; i++) {
        free(ctx->tool_args[i]);
    }
}

static void openai_stream_flush_tools(OpenAIStreamContext *ctx) {
    for (int i = 0; i <= ctx->max_tool_index; i++) {
        if (ctx->tool_ids[i][0] == '\0') continue;  // no tool call at this index

        // Parse accumulated arguments
        char *input_json = NULL;
        if (ctx->tool_args[i] && ctx->tool_args_len[i] > 0) {
            cJSON *parsed = cJSON_Parse(ctx->tool_args[i]);
            if (parsed) {
                input_json = cJSON_PrintUnformatted(parsed);
                cJSON_Delete(parsed);
            } else {
                input_json = strdup(ctx->tool_args[i]);  // raw fallback
            }
        }
        if (!input_json) input_json = strdup("{}");

        StreamChunk chunk = {0};
        chunk.type = CHUNK_TOOL_CALL;
        chunk.tool_name = ctx->tool_names[i];
        chunk.tool_id = ctx->tool_ids[i];
        chunk.tool_input = input_json;
        ctx->callback(&chunk, ctx->userdata);
        free(input_json);

        ctx->tool_ids[i][0] = '\0';  // mark as flushed
    }
}

// OpenAI SSE stream callback
static void openai_stream_callback(const char *line, void *userdata) {
    OpenAIStreamContext *ctx = (OpenAIStreamContext *)userdata;
    if (!line || *line == '\0') return;

    if (strcmp(line, "[DONE]") == 0) {
        openai_stream_flush_tools(ctx);
        if (!ctx->done_sent) {
            StreamChunk chunk = {0};
            chunk.type = CHUNK_DONE;
            ctx->callback(&chunk, ctx->userdata);
            ctx->done_sent = 1;
        }
        return;
    }

    cJSON *json = cJSON_Parse(line);
    if (!json) {
        if (!ctx->non_json_logged && strlen(line) > 10) {
            DEBUG_LOG("[DEBUG][openai_stream] Non-JSON: %.300s\n", line);
            ctx->non_json_logged = 1;
        }
        return;
    }

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    if (!choices || cJSON_GetArraySize(choices) == 0) { cJSON_Delete(json); return; }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice) { cJSON_Delete(json); return; }

    cJSON *delta = cJSON_GetObjectItem(choice, "delta");
    if (!delta) { cJSON_Delete(json); return; }

    // Text content
    cJSON *content = cJSON_GetObjectItem(delta, "content");
    if (content && content->valuestring) {
        StreamChunk chunk = {0};
        chunk.type = CHUNK_CONTENT;
        chunk.content = strdup(content->valuestring);
        ctx->callback(&chunk, ctx->userdata);
        free(chunk.content);
    }

    // Tool calls (OpenAI format: function calls)
    cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (tool_calls) {
        int tc_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tc_count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            if (!tc) continue;

            cJSON *tc_index = cJSON_GetObjectItem(tc, "index");
            int idx = tc_index ? tc_index->valueint : i;
            if (idx < 0 || idx >= 32) continue;
            if (idx > ctx->max_tool_index) ctx->max_tool_index = idx;

            // Capture tool name and id (sent in first chunk of each tool call)
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            if (tc_id && tc_id->valuestring && ctx->tool_ids[idx][0] == '\0') {
                strncpy(ctx->tool_ids[idx], tc_id->valuestring, sizeof(ctx->tool_ids[idx]) - 1);
            }

            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            if (fn) {
                cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
                if (fn_name && fn_name->valuestring && ctx->tool_names[idx][0] == '\0') {
                    strncpy(ctx->tool_names[idx], fn_name->valuestring, sizeof(ctx->tool_names[idx]) - 1);
                }

                // Accumulate arguments (sent incrementally)
                cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
                if (fn_args && fn_args->valuestring) {
                    size_t arg_len = strlen(fn_args->valuestring);
                    size_t needed = ctx->tool_args_len[idx] + arg_len + 1;
                    if (needed > ctx->tool_args_cap[idx]) {
                        ctx->tool_args_cap[idx] = needed * 2;
                        char *new_buf = realloc(ctx->tool_args[idx], ctx->tool_args_cap[idx]);
                        if (!new_buf) { ctx->tool_args_cap[idx] = needed; continue; }
                        ctx->tool_args[idx] = new_buf;
                    }
                    if (ctx->tool_args[idx] && ctx->tool_args_cap[idx] > ctx->tool_args_len[idx] + arg_len) {
                        memcpy(ctx->tool_args[idx] + ctx->tool_args_len[idx], fn_args->valuestring, arg_len);
                        ctx->tool_args_len[idx] += arg_len;
                        ctx->tool_args[idx][ctx->tool_args_len[idx]] = '\0';
                    }
                }
            }
        }
    }

    // Check finish_reason
    cJSON *finish_reason = cJSON_GetObjectItem(choice, "finish_reason");
    if (finish_reason && finish_reason->valuestring && strcmp(finish_reason->valuestring, "stop") == 0) {
        openai_stream_flush_tools(ctx);
        if (!ctx->done_sent) {
            StreamChunk chunk = {0};
            chunk.type = CHUNK_DONE;
            chunk.stop_reason = strdup("stop");
            ctx->callback(&chunk, ctx->userdata);
            free(chunk.stop_reason);
            ctx->done_sent = 1;
        }
    } else if (finish_reason && finish_reason->valuestring && strcmp(finish_reason->valuestring, "tool_calls") == 0) {
        openai_stream_flush_tools(ctx);
        if (!ctx->done_sent) {
            StreamChunk chunk = {0};
            chunk.type = CHUNK_DONE;
            chunk.stop_reason = strdup("tool_calls");
            ctx->callback(&chunk, ctx->userdata);
            free(chunk.stop_reason);
            ctx->done_sent = 1;
        }
    }

    cJSON_Delete(json);
}

// ============================================================================
// Main streaming chat
// ============================================================================

bool provider_chat_stream(Provider *p, cJSON *messages, ChunkCallback callback, void *userdata, ToolRegistry *tools, const volatile long *cancelled, const char *system_prompt) {
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

        if (tools && tools->count > 0) {
            cJSON *tools_arr = cJSON_CreateArray();
            for (int i = 0; i < tools->count; i++) {
                cJSON *tool_def = build_anthropic_tool(tools->tools[i]);
                if (tool_def) cJSON_AddItemToArray(tools_arr, tool_def);
            }
            cJSON_AddItemToObject(body, "tools", tools_arr);
        }

        if (system_prompt && system_prompt[0]) {
            cJSON_AddItemToObject(body, "system", cJSON_CreateString(system_prompt));
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
        cJSON *msgs = cJSON_Duplicate(messages, 1);
        if (system_prompt && system_prompt[0]) {
            msgs = prepend_system_message(msgs, system_prompt);
        }

        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", msgs);
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(p->max_tokens));
        cJSON_AddItemToObject(body, "stream", cJSON_CreateBool(true));

        if (tools && tools->count > 0) {
            cJSON *tools_arr = cJSON_CreateArray();
            for (int i = 0; i < tools->count; i++) {
                cJSON *tool_def = build_openai_tool(tools->tools[i]);
                if (tool_def) cJSON_AddItemToArray(tools_arr, tool_def);
            }
            cJSON_AddItemToObject(body, "tools", tools_arr);
        }

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

    DEBUG_LOG("[DEBUG][chat_stream] Sending to %s (body_len=%zu)\n", url, strlen(json_body));

    {
        FILE *dbg = fopen("debug_request.json", "w");
        if (dbg) { fputs(json_body, dbg); fclose(dbg); }
    }

    bool success;

    if (p->type == PROVIDER_ANTHROPIC) {
        StreamContext ctx;
        ctx.callback = callback;
        ctx.userdata = userdata;
        ctx.current_tool_name[0] = '\0';
        ctx.current_tool_id[0] = '\0';
        ctx.tool_input_buf = NULL;
        ctx.tool_input_len = 0;
        ctx.tool_input_cap = 0;
        ctx.in_tool_use = 0;
        ctx.non_json_logged = 0;

        success = http_post_stream(url, headers, json_body, anthropic_stream_callback, &ctx, 600000, cancelled);
        stream_context_free(&ctx);
    } else {
        OpenAIStreamContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.callback = callback;
        ctx.userdata = userdata;
        ctx.max_tool_index = -1;
        ctx.done_sent = 0;
        ctx.non_json_logged = 0;

        success = http_post_stream(url, headers, json_body, openai_stream_callback, &ctx, 600000, cancelled);
        openai_stream_context_free(&ctx);
    }

    if (!success) {
        DEBUG_LOG("[DEBUG][chat_stream] http_post_stream failed\n");
    }

    free(json_body);
    return success;
}