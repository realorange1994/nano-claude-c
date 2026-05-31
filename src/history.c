#include "history.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void history_init(History *h, int reserve_tokens, int keep_recent_tokens) {
    memset(h, 0, sizeof(History));
    h->reserve_tokens = reserve_tokens;
    h->keep_recent_tokens = keep_recent_tokens;
}

void history_free(History *h) {
    for (int i = 0; i < h->count; i++) {
        free(h->msgs[i].content);
        free(h->msgs[i].tool_id);
        free(h->msgs[i].tool_name);
        free(h->msgs[i].tool_result);
    }
    free(h->summary);
    memset(h, 0, sizeof(History));
}

void history_add(History *h, MessageType type, const char *content) {
    if (h->count >= MAX_MESSAGES) return;

    Message *m = &h->msgs[h->count++];
    memset(m, 0, sizeof(Message));
    m->type = type;
    m->content = content ? strdup(content) : NULL;

    switch (type) {
        case MSG_USER:
            m->role = "user";
            break;
        case MSG_ASSISTANT:
            m->role = "assistant";
            break;
        case MSG_TOOL:
            m->role = "user";
            break;
    }
}

void history_add_tool(History *h, const char *tool_id, const char *tool_name, const char *tool_result) {
    if (h->count >= MAX_MESSAGES) return;
    if (!tool_result) return;

    Message *m = &h->msgs[h->count++];
    memset(m, 0, sizeof(Message));
    m->type = MSG_TOOL;
    m->role = "user";
    m->tool_id = tool_id ? strdup(tool_id) : NULL;
    m->tool_name = tool_name ? strdup(tool_name) : NULL;
    m->tool_result = strdup(tool_result);

    // Build properly escaped JSON using cJSON
    cJSON *content_arr = cJSON_CreateArray();
    cJSON *tool_result_block = cJSON_CreateObject();
    cJSON_AddItemToObject(tool_result_block, "type", cJSON_CreateString("tool_result"));
    cJSON_AddItemToObject(tool_result_block, "tool_use_id", cJSON_CreateString(tool_id ? tool_id : ""));
    cJSON_AddItemToObject(tool_result_block, "content", cJSON_CreateString(tool_result));
    cJSON_AddItemToArray(content_arr, tool_result_block);
    m->content = cJSON_PrintUnformatted(content_arr);
    cJSON_Delete(content_arr);
}

void history_add_tool_call(History *h, const char *tool_id, const char *tool_name, const char *tool_input_json) {
    if (h->count >= MAX_MESSAGES) return;

    Message *m = &h->msgs[h->count++];
    memset(m, 0, sizeof(Message));
    m->type = MSG_ASSISTANT;
    m->role = "assistant";
    m->tool_id = tool_id ? strdup(tool_id) : NULL;
    m->tool_name = tool_name ? strdup(tool_name) : NULL;
    // Store tool input as content (will be parsed when building messages)
    m->content = tool_input_json ? strdup(tool_input_json) : strdup("{}");
}

cJSON *history_to_messages(History *h) {
    cJSON *msgs = cJSON_CreateArray();

    // Add compaction summary if present (like miniclaude)
    if (h->summary && h->summary[0]) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddItemToObject(msg, "role", cJSON_CreateString("system"));
        char *summary_content = malloc(strlen(h->summary) + 50);
        if (summary_content) {
            strcpy(summary_content, "Previous session summary:\n");
            strcat(summary_content, h->summary);
            cJSON_AddItemToObject(msg, "content", cJSON_CreateString(summary_content));
            free(summary_content);
        } else {
            cJSON_AddItemToObject(msg, "content", cJSON_CreateString(h->summary));
        }
        cJSON_AddItemToArray(msgs, msg);
    }

    int i = 0;
    while (i < h->count) {
        Message *m = &h->msgs[i];

        // Skip time context entries (like miniclaude skips EntryTypeSystem)
        if (m->type == MSG_USER && m->content && strncmp(m->content, "[Current time:", 14) == 0) {
            i++;
            continue;
        }

        if (m->type == MSG_TOOL) {
            // Tool result message - user role with tool_result content blocks
            // Group consecutive tool results into one user message (like Go version)
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->msgs[i].type == MSG_TOOL) {
                Message *tm = &h->msgs[i];
                if (tm->content) {
                    cJSON *parsed = cJSON_Parse(tm->content);
                    if (parsed) {
                        // parsed is an array of tool_result blocks, add each
                        int arr_size = cJSON_GetArraySize(parsed);
                        for (int j = 0; j < arr_size; j++) {
                            cJSON *item = cJSON_DetachItemFromArray(parsed, j);
                            cJSON_AddItemToArray(content_arr, item);
                        }
                        cJSON_Delete(parsed);
                    }
                }
                i++;
            }
            cJSON_AddItemToObject(msg, "content", content_arr);
            cJSON_AddItemToArray(msgs, msg);
        } else if (m->type == MSG_ASSISTANT) {
            // Group consecutive assistant messages into one (like Go version)
            // This handles: text + tool_call(s) all in one assistant message
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("assistant"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->msgs[i].type == MSG_ASSISTANT) {
                Message *am = &h->msgs[i];
                if (am->tool_name) {
                    // This is a tool_use entry
                    cJSON *tool_use_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(tool_use_block, "type", cJSON_CreateString("tool_use"));
                    cJSON_AddItemToObject(tool_use_block, "id", cJSON_CreateString(am->tool_id ? am->tool_id : ""));
                    cJSON_AddItemToObject(tool_use_block, "name", cJSON_CreateString(am->tool_name));
                    cJSON *input = NULL;
                    if (am->content) {
                        input = cJSON_Parse(am->content);
                    }
                    if (!input) {
                        input = cJSON_CreateObject();
                    }
                    cJSON_AddItemToObject(tool_use_block, "input", input);
                    cJSON_AddItemToArray(content_arr, tool_use_block);
                } else if (am->content && am->content[0] != '\0') {
                    // This is a text entry
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(text_block, "type", cJSON_CreateString("text"));
                    cJSON_AddItemToObject(text_block, "text", cJSON_CreateString(am->content));
                    cJSON_AddItemToArray(content_arr, text_block);
                }
                i++;
            }
            cJSON_AddItemToObject(msg, "content", content_arr);
            cJSON_AddItemToArray(msgs, msg);
        } else {
            // User message (plain text)
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString(m->role));
            if (m->content) {
                cJSON_AddItemToObject(msg, "content", cJSON_CreateString(m->content));
            } else {
                cJSON_AddItemToObject(msg, "content", cJSON_CreateString(""));
            }
            cJSON_AddItemToArray(msgs, msg);
            i++;
        }
    }

    return msgs;
}

static int estimate_string_tokens(const char *str) {
    if (!str) return 0;
    return (int)(strlen(str) / 4);
}

int history_estimate_tokens(History *h) {
    int tokens = 0;

    for (int i = 0; i < h->count; i++) {
        tokens += estimate_string_tokens(h->msgs[i].content);
        tokens += estimate_string_tokens(h->msgs[i].tool_name);
        tokens += estimate_string_tokens(h->msgs[i].tool_result);
    }

    if (h->summary) {
        tokens += estimate_string_tokens(h->summary);
    }

    return tokens;
}

bool history_needs_compact(History *h) {
    return history_estimate_tokens(h) > h->reserve_tokens;
}

void history_compact(History *h, char *(*summarize_fn)(const char *)) {
    if (!h->summary) {
        h->summary = strdup("");
    }

    Buffer old_content;
    buffer_init(&old_content);

    for (int i = 0; i < h->count; i++) {
        if (h->msgs[i].content) {
            buffer_append_str(&old_content, h->msgs[i].content);
            buffer_append_str(&old_content, "\n");
        }
    }

    char *summary = (*summarize_fn)(buffer_c_str(&old_content));
    buffer_free(&old_content);

    free(h->summary);
    h->summary = summary;

    // Keep only recent messages
    int keep = h->keep_recent_tokens > 0 ? 4 : 0;
    for (int i = keep; i < h->count; i++) {
        free(h->msgs[i].content);
        free(h->msgs[i].tool_id);
        free(h->msgs[i].tool_name);
        free(h->msgs[i].tool_result);
        memset(&h->msgs[i], 0, sizeof(Message));
    }

    if (keep > 0 && h->count > keep) {
        int shift = h->count - keep;
        for (int i = 0; i < keep; i++) {
            h->msgs[i] = h->msgs[i + shift];
            memset(&h->msgs[i + shift], 0, sizeof(Message));
        }
    }
    h->count = keep;
}
