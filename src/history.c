#include "history.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ============================================================================
// Internal helpers
// ============================================================================

static int id_counter = 0;

void history_generate_id(char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "entry_%d_%ld", ++id_counter, (long)time(NULL));
}

char *history_truncate_content(const char *content, int max_len) {
    if (!content) return strdup("");
    int len = strlen(content);
    if (len <= max_len) return strdup(content);
    char *result = malloc(max_len + 20);
    if (result) {
        memcpy(result, content, max_len);
        strcpy(result + max_len, "\n...[truncated]...");
    }
    return result;
}

const char *history_get_summarization_prompt(void) {
    return
"The messages above are a conversation to summarize. Create a structured context checkpoint summary.\n\n"
"## Goal\n[What is the user trying to accomplish?]\n\n"
"## Progress\n- [x] [Completed tasks]\n- [ ] [Current work]\n\n"
"## Next Steps\n1. [Ordered list]\n\n"
"## Critical Context\n- [Data, references]\n";
}

// ============================================================================
// History lifecycle
// ============================================================================

void history_init(History *h, int reserve_tokens, int keep_recent_tokens, int context_window) {
    memset(h, 0, sizeof(History));
    h->reserve_tokens = reserve_tokens;
    h->keep_recent_tokens = keep_recent_tokens;
    h->context_window = context_window;
    h->last_compaction_index = -1;
}

void history_free(History *h) {
    for (int i = 0; i < h->count; i++) {
        free(h->entries[i].content);
        free(h->entries[i].tool_name);
        free(h->entries[i].tool_result);
    }
    // Free file sets
    for (int i = 0; i < h->read_files.count; i++) free(h->read_files.paths[i]);
    for (int i = 0; i < h->written_files.count; i++) free(h->written_files.paths[i]);
    for (int i = 0; i < h->edited_files.count; i++) free(h->edited_files.paths[i]);
    free(h->summary);
    memset(h, 0, sizeof(History));
}

void history_reset(History *h) {
    for (int i = 0; i < h->count; i++) {
        free(h->entries[i].content);
        free(h->entries[i].tool_name);
        free(h->entries[i].tool_result);
    }
    // Free file sets
    for (int i = 0; i < h->read_files.count; i++) free(h->read_files.paths[i]);
    for (int i = 0; i < h->written_files.count; i++) free(h->written_files.paths[i]);
    for (int i = 0; i < h->edited_files.count; i++) free(h->edited_files.paths[i]);
    memset(h, 0, sizeof(History));
    // Re-init with same config
    h->last_compaction_index = -1;
}

static void history_add_entry(History *h, EntryType type, Role role, const char *content) {
    if (h->count >= MAX_MESSAGES) return;

    Entry *e = &h->entries[h->count++];
    memset(e, 0, sizeof(Entry));
    e->type = type;
    e->role = role;
    e->content = content ? strdup(content) : NULL;
    history_generate_id(e->id, sizeof(e->id));
}

// ============================================================================
// Message adding
// ============================================================================

void history_add_user(History *h, const char *content) {
    history_add_entry(h, ENTRY_MESSAGE, ROLE_USER, content);
}

void history_add_assistant(History *h, const char *content) {
    history_add_entry(h, ENTRY_MESSAGE, ROLE_ASSISTANT, content);
}

void history_add_tool_call(History *h, const char *tool_id, const char *tool_name, const char *tool_input_json) {
    if (h->count >= MAX_MESSAGES) return;

    Entry *e = &h->entries[h->count++];
    memset(e, 0, sizeof(Entry));
    e->type = ENTRY_MESSAGE;
    e->role = ROLE_ASSISTANT;
    // Save the original tool_call ID in parent_id (don't overwrite with generated ID)
    strncpy(e->parent_id, tool_id ? tool_id : "", sizeof(e->parent_id) - 1);
    e->tool_name = tool_name ? strdup(tool_name) : NULL;
    e->content = tool_input_json ? strdup(tool_input_json) : strdup("{}");
    // Generate a new unique ID for this entry (separate from tool_call_id)
    history_generate_id(e->id, sizeof(e->id));
}

void history_add_tool_result(History *h, const char *parent_id, const char *tool_name, const char *tool_result) {
    if (h->count >= MAX_MESSAGES) return;
    if (!tool_result) return;

    Entry *e = &h->entries[h->count++];
    memset(e, 0, sizeof(Entry));
    e->type = ENTRY_TOOL_RESULT;
    e->role = ROLE_TOOL;
    if (parent_id) strncpy(e->parent_id, parent_id, sizeof(e->parent_id) - 1);
    e->tool_name = tool_name ? strdup(tool_name) : NULL;

    // Truncate tool result to prevent context overflow
    // Use MAX_TOOL_RESULT_LEN characters + truncation suffix
    size_t result_len = strlen(tool_result);
    if (result_len > MAX_TOOL_RESULT_LEN) {
        char *truncated = malloc(MAX_TOOL_RESULT_LEN + 32);
        if (truncated) {
            memcpy(truncated, tool_result, MAX_TOOL_RESULT_LEN);
            strcpy(truncated + MAX_TOOL_RESULT_LEN, "\n... [truncated] ...");
        }
        e->tool_result = truncated ? truncated : strdup("[output too large]");
    } else {
        e->tool_result = strdup(tool_result);
    }
    history_generate_id(e->id, sizeof(e->id));
}

void history_add_system(History *h, const char *content) {
    history_add_entry(h, ENTRY_SYSTEM, ROLE_USER, content);
}

// ============================================================================
// Time context
// ============================================================================

void history_inject_time_context(History *h) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), "[Current time: %Y-%m-%d %H:%M:%S]", t);

    // Update existing time entry if present
    for (int i = h->count - 1; i >= 0; i--) {
        if (h->entries[i].type == ENTRY_SYSTEM && h->entries[i].content &&
            strncmp(h->entries[i].content, "[Current time:", 14) == 0) {
            free(h->entries[i].content);
            h->entries[i].content = strdup(timebuf);
            return;
        }
    }
    // Add new time entry
    history_add_system(h, timebuf);
}

// ============================================================================
// Token estimation
// ============================================================================

static int estimate_string_tokens(const char *str) {
    if (!str) return 0;
    int chars = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p < 0x80) {
            chars++;
        } else if (*p >= 0xC0) {
            if (*p >= 0xF0) chars += 4;
            else if (*p >= 0xE0) chars += 3;
            else chars += 2;
        }
        p++;
    }
    return chars;
}

int history_estimate_tokens(History *h) {
    int tokens = 0;
    for (int i = 0; i < h->count; i++) {
        tokens += estimate_string_tokens(h->entries[i].content);
        tokens += estimate_string_tokens(h->entries[i].tool_name);
        tokens += estimate_string_tokens(h->entries[i].tool_result);
    }
    if (h->summary) {
        tokens += estimate_string_tokens(h->summary);
    }
    return tokens;
}

bool history_needs_compact(History *h) {
    // Threshold: context_window - reserve_tokens - keep_recent_tokens/2
    // This gives more room before triggering compact
    int threshold = h->context_window - h->reserve_tokens - (h->keep_recent_tokens / 2);
    if (threshold < h->context_window / 2) {
        threshold = h->context_window / 2;  // At least 50% of context window
    }
    return history_estimate_tokens(h) > threshold;
}

// ============================================================================
// Message building for API
// ============================================================================

cJSON *history_to_messages(History *h) {
    cJSON *msgs = cJSON_CreateArray();

    // Add compaction summary if present
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
        Entry *e = &h->entries[i];

        // Skip system entries
        if (e->type == ENTRY_SYSTEM) {
            i++;
            continue;
        }

        if (e->type == ENTRY_TOOL_RESULT) {
            // Tool result message - user role with tool_result content blocks
            // Group consecutive tool results into one user message
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->entries[i].type == ENTRY_TOOL_RESULT) {
                Entry *te = &h->entries[i];
                if (te->tool_result) {
                    cJSON *tool_result_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(tool_result_block, "type", cJSON_CreateString("tool_result"));
                    cJSON_AddItemToObject(tool_result_block, "tool_use_id",
                        cJSON_CreateString(te->parent_id[0] ? te->parent_id : ""));
                    cJSON_AddItemToObject(tool_result_block, "content",
                        cJSON_CreateString(te->tool_result));
                    cJSON_AddItemToArray(content_arr, tool_result_block);
                }
                i++;
            }
            cJSON_AddItemToObject(msg, "content", content_arr);
            cJSON_AddItemToArray(msgs, msg);
        } else if (e->role == ROLE_ASSISTANT) {
            // Assistant message - group consecutive assistant entries (text + tool_use)
            // into one assistant message with content blocks
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("assistant"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->entries[i].role == ROLE_ASSISTANT) {
                Entry *ae = &h->entries[i];
                if (ae->tool_name) {
                    // Tool use block - use parent_id as the tool_call ID
                    cJSON *tool_use_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(tool_use_block, "type", cJSON_CreateString("tool_use"));
                    cJSON_AddItemToObject(tool_use_block, "id",
                        cJSON_CreateString(ae->parent_id[0] ? ae->parent_id : ae->id));
                    cJSON_AddItemToObject(tool_use_block, "name", cJSON_CreateString(ae->tool_name));
                    cJSON *input = NULL;
                    if (ae->content) {
                        input = cJSON_Parse(ae->content);
                    }
                    if (!input) input = cJSON_CreateObject();
                    cJSON_AddItemToObject(tool_use_block, "input", input);
                    cJSON_AddItemToArray(content_arr, tool_use_block);
                } else if (ae->content && ae->content[0] != '\0') {
                    // Text block
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(text_block, "type", cJSON_CreateString("text"));
                    cJSON_AddItemToObject(text_block, "text", cJSON_CreateString(ae->content));
                    cJSON_AddItemToArray(content_arr, text_block);
                }
                i++;
            }
            cJSON_AddItemToObject(msg, "content", content_arr);
            cJSON_AddItemToArray(msgs, msg);
        } else {
            // User message (plain text)
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
            if (e->content) {
                cJSON_AddItemToObject(msg, "content", cJSON_CreateString(e->content));
            } else {
                cJSON_AddItemToObject(msg, "content", cJSON_CreateString(""));
            }
            cJSON_AddItemToArray(msgs, msg);
            i++;
        }
    }

    return msgs;
}

// ============================================================================
// Compaction
// ============================================================================

int history_find_cut_point(History *h) {
    // Find the last entry before the keep_recent_tokens limit
    int keep = h->keep_recent_tokens > 0 ? 4 : 0;
    int cut_point = h->count - keep;
    if (cut_point < 1) cut_point = 1;

    // Try to find a good boundary (prefer ending at a user message or tool result)
    for (int i = cut_point; i < h->count; i++) {
        if (h->entries[i].type == ENTRY_MESSAGE && h->entries[i].role == ROLE_USER) {
            return i;
        }
        if (h->entries[i].type == ENTRY_TOOL_RESULT) {
            return i + 1;
        }
    }
    return cut_point;
}

void history_compact(History *h, char *(*summarize_fn)(const char *)) {
    if (!h->summary) {
        h->summary = strdup("");
    }

    Buffer old_content;
    buffer_init(&old_content);

    for (int i = 0; i < h->count; i++) {
        if (h->entries[i].content) {
            buffer_append_str(&old_content, h->entries[i].content);
            buffer_append_str(&old_content, "\n");
        }
    }

    char *summary = (*summarize_fn)(buffer_c_str(&old_content));
    buffer_free(&old_content);

    free(h->summary);
    h->summary = summary;

    if (summary && summary[0]) {
        // Find cut point
        int cut_point = history_find_cut_point(h);

        // Free entries before cut point
        for (int i = 0; i < cut_point; i++) {
            free(h->entries[i].content);
            free(h->entries[i].tool_name);
            free(h->entries[i].tool_result);
        }

        // Shift remaining entries to front
        int keep_count = h->count - cut_point;
        for (int i = 0; i < keep_count; i++) {
            h->entries[i] = h->entries[cut_point + i];
        }
        // Clear old entries
        for (int i = keep_count; i < h->count; i++) {
            memset(&h->entries[i], 0, sizeof(Entry));
        }
        h->count = keep_count;
        h->last_compaction_index = h->count - 1;
    } else {
        free(summary);
        fprintf(stderr, "[Compaction failed: no summary generated]\n");
    }
}

// ============================================================================
// File operation tracking
// ============================================================================