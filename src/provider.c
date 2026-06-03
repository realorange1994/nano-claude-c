#include "provider.h"
#include "tool.h"
#include "http.h"
#include "buffer.h"
#include "config.h"
#include "retry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// URL construction helpers
// ============================================================================

// Build full URL: append /v1/messages or /v1/chat/completions to base_url,
// avoiding duplicate /v1 if base_url already ends with the suffix.
static void build_anthropic_url(char *url, size_t url_size, const char *base_url) {
    if (!base_url) {
        snprintf(url, url_size, "https://api.anthropic.com/v1/messages");
        return;
    }
    size_t len = strlen(base_url);
    if (strstr(base_url, "/v1/messages")) {
        snprintf(url, url_size, "%s", base_url);
    } else if (len >= 3 && strcmp(base_url + len - 3, "/v1") == 0) {
        snprintf(url, url_size, "%s/messages", base_url);
    } else {
        snprintf(url, url_size, "%s/v1/messages", base_url);
    }
}

static void build_openai_url(char *url, size_t url_size, const char *base_url) {
    if (!base_url) {
        snprintf(url, url_size, "https://api.openai.com/v1/chat/completions");
        return;
    }
    size_t len = strlen(base_url);
    if (strstr(base_url, "/v1/chat/completions")) {
        snprintf(url, url_size, "%s", base_url);
    } else if (len >= 3 && strcmp(base_url + len - 3, "/v1") == 0) {
        snprintf(url, url_size, "%s/chat/completions", base_url);
    } else {
        snprintf(url, url_size, "%s/v1/chat/completions", base_url);
    }
}

struct Provider {
    ProviderType type;
    char *api_key;
    char *model;
    char *base_url;
    int max_tokens;
    RetryState retry_state;
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
    retry_state_init(&p->retry_state);
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

void provider_reset_retry_state(Provider *p) {
    if (p) retry_state_init(&p->retry_state);
}

void provider_reset_stream_failure(Provider *p) {
    if (p) retry_state_stream_success(&p->retry_state);
}

void provider_get_last_error(Provider *p, int *http_status, char *error_msg, size_t error_msg_size) {
    if (!p || !http_status || !error_msg) return;
    *http_status = p->retry_state.last_error_status;
    if (error_msg && error_msg_size > 0) {
        strncpy(error_msg, p->retry_state.last_error, error_msg_size - 1);
        error_msg[error_msg_size - 1] = '\0';
    }
}

// ============================================================================
// Message format conversion: Anthropic-style -> OpenAI-style
// history_to_messages() produces Anthropic format (tool_use/tool_result content blocks).
// OpenAI API needs: role=assistant+tool_calls array, role=tool for results.
// ============================================================================

static cJSON *convert_to_openai_messages(cJSON *anthropic_msgs) {
    if (!anthropic_msgs) return NULL;

    cJSON *openai_msgs = cJSON_CreateArray();
    int msg_count = cJSON_GetArraySize(anthropic_msgs);

    for (int mi = 0; mi < msg_count; mi++) {
        cJSON *msg = cJSON_GetArrayItem(anthropic_msgs, mi);
        if (!msg) continue;

        cJSON *role_item = cJSON_GetObjectItem(msg, "role");
        if (!role_item || !role_item->valuestring) continue;
        const char *role = role_item->valuestring;

        cJSON *content = cJSON_GetObjectItem(msg, "content");

        if (strcmp(role, "assistant") == 0) {
            // Check if content is an array (Anthropic content blocks)
            if (content && content->type == cJSON_Array) {
                // Extract text and tool_calls separately
                cJSON *oai_msg = cJSON_CreateObject();
                cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("assistant"));

                cJSON *tool_calls = cJSON_CreateArray();
                Buffer text_buf;
                buffer_init(&text_buf);

                int block_count = cJSON_GetArraySize(content);
                for (int bi = 0; bi < block_count; bi++) {
                    cJSON *block = cJSON_GetArrayItem(content, bi);
                    if (!block) continue;
                    cJSON *btype = cJSON_GetObjectItem(block, "type");
                    if (!btype || !btype->valuestring) continue;

                    if (strcmp(btype->valuestring, "text") == 0) {
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (text && text->valuestring) {
                            buffer_append_str(&text_buf, text->valuestring);
                        }
                    } else if (strcmp(btype->valuestring, "tool_use") == 0) {
                        // Convert to OpenAI tool_call format
                        cJSON *tc = cJSON_CreateObject();
                        cJSON *id = cJSON_GetObjectItem(block, "id");
                        cJSON *name = cJSON_GetObjectItem(block, "name");
                        cJSON *input = cJSON_GetObjectItem(block, "input");

                        cJSON_AddItemToObject(tc, "id",
                            id ? cJSON_CreateString(id->valuestring) : cJSON_CreateString(""));
                        cJSON_AddItemToObject(tc, "type", cJSON_CreateString("function"));

                        cJSON *fn = cJSON_CreateObject();
                        cJSON_AddItemToObject(fn, "name",
                            name ? cJSON_CreateString(name->valuestring) : cJSON_CreateString(""));
                        char *arg_str = input ? cJSON_PrintUnformatted(input) : strdup("{}");
                        cJSON_AddItemToObject(fn, "arguments", cJSON_CreateString(arg_str));
                        free(arg_str);
                        cJSON_AddItemToObject(tc, "function", fn);
                        cJSON_AddItemToArray(tool_calls, tc);
                    }
                }

                if (text_buf.len > 0) {
                    cJSON_AddItemToObject(oai_msg, "content", cJSON_CreateString(buffer_c_str(&text_buf)));
                } else {
                    cJSON_AddNullToObject(oai_msg, "content");
                }
                buffer_free(&text_buf);

                if (cJSON_GetArraySize(tool_calls) > 0) {
                    cJSON_AddItemToObject(oai_msg, "tool_calls", tool_calls);
                } else {
                    cJSON_Delete(tool_calls);
                }

                cJSON_AddItemToArray(openai_msgs, oai_msg);
            } else if (content && content->valuestring) {
                // Simple string content
                cJSON *oai_msg = cJSON_CreateObject();
                cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("assistant"));
                cJSON_AddItemToObject(oai_msg, "content", cJSON_CreateString(content->valuestring));
                cJSON_AddItemToArray(openai_msgs, oai_msg);
            } else {
                // No content
                cJSON *oai_msg = cJSON_CreateObject();
                cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("assistant"));
                cJSON_AddNullToObject(oai_msg, "content");
                cJSON_AddItemToArray(openai_msgs, oai_msg);
            }
        } else if (strcmp(role, "user") == 0) {
            if (content && content->type == cJSON_Array) {
                // Check for tool_result blocks -> split into separate role=tool messages
                int has_tool_result = 0;
                int block_count = cJSON_GetArraySize(content);
                for (int bi = 0; bi < block_count; bi++) {
                    cJSON *block = cJSON_GetArrayItem(content, bi);
                    if (block) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (btype && btype->valuestring && strcmp(btype->valuestring, "tool_result") == 0) {
                            has_tool_result = 1;
                            break;
                        }
                    }
                }

                if (has_tool_result) {
                    // Split: text blocks become user messages, tool_result blocks become tool messages
                    int block_count = cJSON_GetArraySize(content);
                    for (int bi = 0; bi < block_count; bi++) {
                        cJSON *block = cJSON_GetArrayItem(content, bi);
                        if (!block) continue;
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || !btype->valuestring) continue;

                        if (strcmp(btype->valuestring, "tool_result") == 0) {
                            cJSON *oai_tool = cJSON_CreateObject();
                            cJSON_AddItemToObject(oai_tool, "role", cJSON_CreateString("tool"));
                            cJSON *tool_use_id = cJSON_GetObjectItem(block, "tool_use_id");
                            cJSON_AddItemToObject(oai_tool, "tool_call_id",
                                tool_use_id ? cJSON_CreateString(tool_use_id->valuestring) : cJSON_CreateString(""));
                            cJSON *tool_content = cJSON_GetObjectItem(block, "content");
                            cJSON_AddItemToObject(oai_tool, "content",
                                tool_content ? cJSON_CreateString(tool_content->valuestring) : cJSON_CreateString(""));
                            cJSON_AddItemToArray(openai_msgs, oai_tool);
                        } else if (strcmp(btype->valuestring, "text") == 0) {
                            cJSON *text = cJSON_GetObjectItem(block, "text");
                            if (text && text->valuestring) {
                                cJSON *oai_user = cJSON_CreateObject();
                                cJSON_AddItemToObject(oai_user, "role", cJSON_CreateString("user"));
                                cJSON_AddItemToObject(oai_user, "content", cJSON_CreateString(text->valuestring));
                                cJSON_AddItemToArray(openai_msgs, oai_user);
                            }
                        } else {
                            // Other block types (image, etc.) - skip for now
                        }
                    }
                } else {
                    // No tool_result blocks: keep as-is or flatten text blocks
                    // Check if all blocks are text -> flatten to single string
                    int all_text = 1;
                    Buffer text_buf;
                    buffer_init(&text_buf);
                    int block_count = cJSON_GetArraySize(content);
                    for (int bi = 0; bi < block_count; bi++) {
                        cJSON *block = cJSON_GetArrayItem(content, bi);
                        if (!block) { all_text = 0; break; }
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (btype && btype->valuestring && strcmp(btype->valuestring, "text") == 0) {
                            cJSON *text = cJSON_GetObjectItem(block, "text");
                            if (text && text->valuestring) {
                                buffer_append_str(&text_buf, text->valuestring);
                            }
                        } else {
                            all_text = 0;
                            break;
                        }
                    }
                    if (all_text && text_buf.len > 0) {
                        cJSON *oai_msg = cJSON_CreateObject();
                        cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("user"));
                        cJSON_AddItemToObject(oai_msg, "content", cJSON_CreateString(buffer_c_str(&text_buf)));
                        cJSON_AddItemToArray(openai_msgs, oai_msg);
                    } else {
                        // Keep original content array
                        cJSON *oai_msg = cJSON_CreateObject();
                        cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("user"));
                        cJSON_AddItemToObject(oai_msg, "content", cJSON_Duplicate(content, 1));
                        cJSON_AddItemToArray(openai_msgs, oai_msg);
                    }
                    buffer_free(&text_buf);
                }
            } else if (content && content->valuestring) {
                // Simple string user message
                cJSON *oai_msg = cJSON_CreateObject();
                cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("user"));
                cJSON_AddItemToObject(oai_msg, "content", cJSON_CreateString(content->valuestring));
                cJSON_AddItemToArray(openai_msgs, oai_msg);
            } else {
                cJSON *oai_msg = cJSON_CreateObject();
                cJSON_AddItemToObject(oai_msg, "role", cJSON_CreateString("user"));
                cJSON_AddItemToObject(oai_msg, "content", cJSON_CreateString(""));
                cJSON_AddItemToArray(openai_msgs, oai_msg);
            }
        } else {
            // system or other: pass through
            cJSON_AddItemToArray(openai_msgs, cJSON_Duplicate(msg, 1));
        }
    }

    return openai_msgs;
}

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
    const int max_retries = 15;

    // Build request body and headers (done once, reused on retry)
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
        build_anthropic_url(url, sizeof(url), p->base_url);
    } else {
        cJSON *oai_msgs = convert_to_openai_messages(messages);
        if (!oai_msgs) return NULL;
        if (system_prompt && system_prompt[0]) {
            prepend_system_message(oai_msgs, system_prompt);
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", oai_msgs);
        cJSON_AddItemToObject(body, "max_tokens", cJSON_CreateNumber(max_tokens));
        json_body = cJSON_Print(body);
        cJSON_Delete(body);
        snprintf(headers, sizeof(headers),
            "Content-Type: application/json\n"
            "Authorization: Bearer %s\n",
            p->api_key);
        build_openai_url(url, sizeof(url), p->base_url);
    }

    if (!json_body) return NULL;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        HttpResponse resp = http_post_ex(url, headers, json_body, 120000);

        if (resp.body) {
            // Success
            retry_state_on_success(&p->retry_state);
            free(json_body);

            cJSON *json = cJSON_Parse(resp.body);
            free(resp.body);
            if (!json) return NULL;

            char *content = NULL;
            if (p->type == PROVIDER_ANTHROPIC) {
                cJSON *content_arr = cJSON_GetObjectItem(json, "content");
                if (content_arr && cJSON_GetArraySize(content_arr) > 0) {
                    cJSON *item = cJSON_GetArrayItem(content_arr, 0);
                    if (item) {
                        cJSON *text = cJSON_GetObjectItem(item, "text");
                        if (text && text->valuestring) content = strdup(text->valuestring);
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
                            if (msg_content && msg_content->valuestring) content = strdup(msg_content->valuestring);
                        }
                    }
                }
            }
            cJSON_Delete(json);
            return content;
        }

        // Request failed -- classify error
        RetryResult r = classify_error(resp.http_status, resp.error);
        char error_copy[512];
        strncpy(error_copy, resp.error, sizeof(error_copy) - 1);
        error_copy[sizeof(error_copy) - 1] = '\0';
        free(resp.body);

        DEBUG_LOG("[retry][sync] attempt %d/%d failed: HTTP %d, decision=%d: %s\n",
                  attempt + 1, max_retries + 1, resp.http_status, r.decision, error_copy);

        // Non-retryable or context overflow -> stop retrying
        if (r.decision == RETRY_NON_RETRYABLE || r.decision == RETRY_CONTEXT_OVERFLOW) {
            DEBUG_LOG("[retry][sync] non-retryable, stopping\n");
            break;
        }

        // Last attempt -> stop
        if (attempt >= max_retries) break;

        // Wait before retry (use Retry-After if available, else backoff)
        int delay_ms = r.retry_after_secs > 0 ? r.retry_after_secs * 1000
                                               : retry_backoff_ms(attempt);
        DEBUG_LOG("[retry][sync] waiting %dms before retry\n", delay_ms);
        retry_sleep_ms(delay_ms);

        p->retry_state.consecutive_errors = attempt + 1;
        p->retry_state.last_error_status = resp.http_status;
        strncpy(p->retry_state.last_error, error_copy, sizeof(p->retry_state.last_error) - 1);
    }

    free(json_body);
    return NULL;
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

    const int max_retries = 15;

    // Check if streaming fallback should be used (3+ consecutive failures)
    if (retry_state_should_fallback(&p->retry_state)) {
        DEBUG_LOG("[retry][stream] using sync fallback after %d stream failures\n", p->retry_state.stream_failures);
        char *content = provider_chat_sync(p, messages, 0, system_prompt);
        if (content) {
            StreamChunk chunk = {0};
            chunk.type = CHUNK_CONTENT;
            chunk.content = content;
            callback(&chunk, userdata);
            free(content);
        }
        StreamChunk chunk = {0};
        chunk.type = CHUNK_DONE;
        callback(&chunk, userdata);
        retry_state_stream_success(&p->retry_state);
        return content != NULL;
    }

    // Build request body and headers (done once, reused on retry)
    char *json_body = NULL;
    char headers[1024];
    char url[512];

    if (p->type == PROVIDER_ANTHROPIC) {
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

        build_anthropic_url(url, sizeof(url), p->base_url);
    } else {
        cJSON *oai_msgs = convert_to_openai_messages(messages);
        if (!oai_msgs) { free(json_body); return false; }
        if (system_prompt && system_prompt[0]) {
            prepend_system_message(oai_msgs, system_prompt);
        }

        cJSON *body = cJSON_CreateObject();
        cJSON_AddItemToObject(body, "model", cJSON_CreateString(p->model));
        cJSON_AddItemToObject(body, "messages", oai_msgs);
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

        build_openai_url(url, sizeof(url), p->base_url);
    }

    if (!json_body) return false;

    DEBUG_LOG("[DEBUG][chat_stream] Sending to %s (body_len=%zu)\n", url, strlen(json_body));

    {
        FILE *dbg = fopen("debug_request.json", "w");
        if (dbg) { fputs(json_body, dbg); fclose(dbg); }
    }

    bool success = false;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
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

            HttpStreamResult stream_result;
            success = http_post_stream_ex(url, headers, json_body, anthropic_stream_callback, &ctx, 600000, cancelled, &stream_result);
            stream_context_free(&ctx);

            if (!success) {
                // Ensure the stream is properly terminated for the callback
                StreamChunk done_chunk = {0};
                done_chunk.type = CHUNK_DONE;
                callback(&done_chunk, userdata);

                DEBUG_LOG("[retry][stream] attempt %d/%d failed: HTTP %d: %s\n",
                          attempt + 1, max_retries + 1, stream_result.http_status, stream_result.error);
                RetryResult r = classify_error(stream_result.http_status, stream_result.error);
                if (r.decision == RETRY_NON_RETRYABLE || r.decision == RETRY_CONTEXT_OVERFLOW) {
                    DEBUG_LOG("[retry][stream] non-retryable, stopping\n");
                    break;
                }
                if (attempt < max_retries) {
                    int delay_ms = r.retry_after_secs > 0 ? r.retry_after_secs * 1000
                                                          : retry_backoff_ms(attempt);
                    DEBUG_LOG("[retry][stream] waiting %dms\n", delay_ms);
                    retry_sleep_ms(delay_ms);
                }
                p->retry_state.consecutive_errors = attempt + 1;
                p->retry_state.last_error_status = stream_result.http_status;
                strncpy(p->retry_state.last_error, stream_result.error, sizeof(p->retry_state.last_error) - 1);
            } else {
                retry_state_on_success(&p->retry_state);
                retry_state_stream_success(&p->retry_state);
                break;
            }
        } else {
            OpenAIStreamContext ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.callback = callback;
            ctx.userdata = userdata;
            ctx.max_tool_index = -1;
            ctx.done_sent = 0;
            ctx.non_json_logged = 0;

            HttpStreamResult stream_result;
            success = http_post_stream_ex(url, headers, json_body, openai_stream_callback, &ctx, 600000, cancelled, &stream_result);
            openai_stream_context_free(&ctx);

            if (!success) {
                // Ensure the stream is properly terminated for the callback
                StreamChunk done_chunk = {0};
                done_chunk.type = CHUNK_DONE;
                callback(&done_chunk, userdata);

                DEBUG_LOG("[retry][stream] attempt %d/%d failed: HTTP %d: %s\n",
                          attempt + 1, max_retries + 1, stream_result.http_status, stream_result.error);
                RetryResult r = classify_error(stream_result.http_status, stream_result.error);
                if (r.decision == RETRY_NON_RETRYABLE || r.decision == RETRY_CONTEXT_OVERFLOW) {
                    DEBUG_LOG("[retry][stream] non-retryable, stopping\n");
                    break;
                }
                if (attempt < max_retries) {
                    int delay_ms = r.retry_after_secs > 0 ? r.retry_after_secs * 1000
                                                          : retry_backoff_ms(attempt);
                    DEBUG_LOG("[retry][stream] waiting %dms\n", delay_ms);
                    retry_sleep_ms(delay_ms);
                }
                p->retry_state.consecutive_errors = attempt + 1;
                p->retry_state.last_error_status = stream_result.http_status;
                strncpy(p->retry_state.last_error, stream_result.error, sizeof(p->retry_state.last_error) - 1);
            } else {
                retry_state_on_success(&p->retry_state);
                retry_state_stream_success(&p->retry_state);
                break;
            }
        }
    }

    // Ensure stream is always terminated (handles case where server sends partial data
    // then closes connection without [DONE], or retries exhausted)
    {
        StreamChunk done_chunk = {0};
        done_chunk.type = CHUNK_DONE;
        callback(&done_chunk, userdata);
    }

    free(json_body);
    return success;
}