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
    e->parent_id[sizeof(e->parent_id) - 1] = '\0';
    e->tool_name = tool_name ? strdup(tool_name) : NULL;
    e->content = tool_input_json ? strdup(tool_input_json) : strdup("{}");
    // Generate a new unique ID for this entry (separate from tool_call_id)
    history_generate_id(e->id, sizeof(e->id));
}

void history_add_tool_result(History *h, const char *parent_id, const char *tool_name, const char *tool_result) {
    if (h->count >= MAX_MESSAGES) return;
    if (!tool_result) {
        // Handle NULL tool_result gracefully
        Entry *e = &h->entries[h->count++];
        memset(e, 0, sizeof(Entry));
        e->type = ENTRY_TOOL_RESULT;
        e->role = ROLE_TOOL;
        if (parent_id) strncpy(e->parent_id, parent_id, sizeof(e->parent_id) - 1);
        e->tool_name = tool_name ? strdup(tool_name) : NULL;
        e->tool_result = strdup("[empty result]");
        history_generate_id(e->id, sizeof(e->id));
        return;
    }

    Entry *e = &h->entries[h->count++];
    memset(e, 0, sizeof(Entry));
    e->type = ENTRY_TOOL_RESULT;
    e->role = ROLE_TOOL;
    if (parent_id) strncpy(e->parent_id, parent_id, sizeof(e->parent_id) - 1);
    e->tool_name = tool_name ? strdup(tool_name) : NULL;

    // Truncate tool result to prevent context overflow
    size_t result_len = strlen(tool_result);
    if (result_len > MAX_TOOL_RESULT_LEN) {
        char *truncated = malloc(MAX_TOOL_RESULT_LEN + 32);
        if (truncated) {
            memcpy(truncated, tool_result, MAX_TOOL_RESULT_LEN);
            truncated[MAX_TOOL_RESULT_LEN] = '\0';
            strcat(truncated, "\n... [truncated] ...");
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

static int estimate_entry_tokens(Entry *e) {
    if (e->role == ROLE_ASSISTANT && e->tool_name) {
        // Tool call: tool input JSON + overhead
        return estimate_string_tokens(e->content) + 15;
    }
    if (e->type == ENTRY_TOOL_RESULT) {
        // Tool result: result content + overhead
        return estimate_string_tokens(e->tool_result) + 10;
    }
    if (e->role == ROLE_USER || e->role == ROLE_ASSISTANT) {
        return estimate_string_tokens(e->content) + 10;
    }
    return 10; // system or other
}

int history_estimate_tokens(History *h) {
    int tokens = 0;
    for (int i = 0; i < h->count; i++) {
        tokens += estimate_entry_tokens(&h->entries[i]);
    }
    if (h->summary) {
        tokens += estimate_string_tokens(h->summary) + 10;
    }
    return tokens;
}

bool history_needs_compact(History *h) {
    // Pi-style: trigger when tokens > contextWindow - reserveTokens
    int reserve = h->reserve_tokens;
    if (reserve <= 0) reserve = 16384;
    return history_estimate_tokens(h) > h->context_window - reserve;
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

// Build list of valid cut points (entry indices where we can safely cut)
// We can cut at user messages, assistant messages, or after tool results
// Returns count of cut points stored in cut_points (max MAX_CUT_POINTS)
#define MAX_CUT_POINTS 2048

static int build_cut_points(History *h, int *cut_points, int *count) {
    *count = 0;
    for (int i = 0; i < h->count && *count < MAX_CUT_POINTS; i++) {
        Entry *e = &h->entries[i];
        switch (e->type) {
        case ENTRY_MESSAGE:
            cut_points[*count] = i; (*count)++; break;
        case ENTRY_TOOL_RESULT:
            // Can cut after a tool result
            if (i + 1 < h->count) {
                cut_points[*count] = i + 1; (*count)++;
            }
            break;
        default: break;
        }
    }
    return *count;
}

int history_find_cut_point(History *h) {
    if (h->count == 0) return 0;

    int keep_recent = h->keep_recent_tokens > 0 ? h->keep_recent_tokens : 20000;
    int cut_points[MAX_CUT_POINTS];
    int num_cut = 0;
    build_cut_points(h, cut_points, &num_cut);

    if (num_cut == 0) {
        // Fallback: find first tool result or return 0
        for (int i = 0; i < h->count; i++) {
            if (h->entries[i].type == ENTRY_TOOL_RESULT) return i;
        }
        return 0;
    }

    // Walk backwards from newest entry, accumulating tokens
    int accumulated = 0;
    for (int i = h->count - 1; i >= 0; i--) {
        accumulated += estimate_entry_tokens(&h->entries[i]);
        if (accumulated >= keep_recent) {
            // Find closest cut point at or after this index
            for (int j = 0; j < num_cut; j++) {
                if (cut_points[j] >= i) {
                    // Don't cut at a tool result (must follow its tool call)
                    if (cut_points[j] < h->count &&
                        h->entries[cut_points[j]].type == ENTRY_TOOL_RESULT) {
                        continue;
                    }
                    return cut_points[j];
                }
            }
        }
    }

    return 0;
}

// Remove orphaned tool results whose tool call was cut away
static void remove_orphaned_tool_results(History *h) {
    // Build set of valid tool call IDs
    int valid_count = 0;
    char valid_ids[MAX_CUT_POINTS][64];
    for (int i = 0; i < h->count && valid_count < MAX_CUT_POINTS; i++) {
        Entry *e = &h->entries[i];
        if (e->type == ENTRY_MESSAGE && e->role == ROLE_ASSISTANT && e->tool_name && e->tool_name[0]) {
            strncpy(valid_ids[valid_count], e->parent_id[0] ? e->parent_id : e->id, 63);
            valid_ids[valid_count][63] = '\0';
            valid_count++;
        }
    }

    // Compact out orphaned tool results
    int write = 0;
    for (int read = 0; read < h->count; read++) {
        Entry *e = &h->entries[read];
        if (e->type == ENTRY_TOOL_RESULT) {
            // Check if parent_id matches any valid tool call ID
            int found = 0;
            for (int v = 0; v < valid_count; v++) {
                if (strcmp(e->parent_id, valid_ids[v]) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                // Orphaned - free and skip
                free(e->content);
                free(e->tool_name);
                free(e->tool_result);
                continue;
            }
        }
        if (write != read) {
            h->entries[write] = h->entries[read];
        }
        write++;
    }
    // Clear old entries
    for (int i = write; i < h->count; i++) {
        memset(&h->entries[i], 0, sizeof(Entry));
    }
    h->count = write;
}

void history_compact(History *h, char *(*summarize_fn)(const char *)) {
    if (h->count == 0) return;

    // Build old content for summarization (entries before cut)
    Buffer old_content;
    buffer_init(&old_content);

    for (int i = 0; i < h->count; i++) {
        if (h->entries[i].content) {
            buffer_append_str(&old_content, h->entries[i].content);
            buffer_append_str(&old_content, "\n");
        }
        if (h->entries[i].tool_result) {
            buffer_append_str(&old_content, "[TOOL: ");
            buffer_append_str(&old_content, h->entries[i].tool_name ? h->entries[i].tool_name : "");
            buffer_append_str(&old_content, "]: ");
            buffer_append_str(&old_content, h->entries[i].tool_result);
            buffer_append_str(&old_content, "\n");
        }
    }

    char *summary = (*summarize_fn)(buffer_c_str(&old_content));
    buffer_free(&old_content);

    if (!summary || !summary[0]) {
        free(summary);
        fprintf(stderr, "[Compaction failed: no summary generated]\n");
        return;
    }

    // Store the summary BEFORE replacing entries
    free(h->summary);
    h->summary = summary;

    // Find cut point
    int cut_index = history_find_cut_point(h);
    if (cut_index >= h->count) cut_index = h->count - 1;
    if (cut_index == 0) return; // Nothing to compact

    // Keep only entries after the cut
    int keep_count = h->count - cut_index;
    for (int i = 0; i < keep_count; i++) {
        h->entries[i] = h->entries[cut_index + i];
    }
    // Clear old entries
    for (int i = keep_count; i < h->count; i++) {
        memset(&h->entries[i], 0, sizeof(Entry));
    }
    h->count = keep_count;

    // Remove orphaned tool results whose tool call was cut away
    remove_orphaned_tool_results(h);

    // Rebuild file operations from remaining entries
    h->read_files.count = 0;
    h->written_files.count = 0;
    h->edited_files.count = 0;
    for (int i = 0; i < h->count; i++) {
        Entry *e = &h->entries[i];
        if (e->type == ENTRY_MESSAGE && e->role == ROLE_ASSISTANT && e->tool_name) {
            cJSON *input = cJSON_Parse(e->content);
            if (input) {
                cJSON *path_item = cJSON_GetObjectItem(input, "file_path");
                if (path_item && path_item->valuestring) {
                    if (strcmp(e->tool_name, "Read") == 0) {
                        if (h->read_files.count < MAX_FILE_PATHS)
                            h->read_files.paths[h->read_files.count++] = strdup(path_item->valuestring);
                    } else if (strcmp(e->tool_name, "Write") == 0) {
                        if (h->written_files.count < MAX_FILE_PATHS)
                            h->written_files.paths[h->written_files.count++] = strdup(path_item->valuestring);
                    } else if (strcmp(e->tool_name, "Edit") == 0) {
                        if (h->edited_files.count < MAX_FILE_PATHS)
                            h->edited_files.paths[h->edited_files.count++] = strdup(path_item->valuestring);
                    }
                }
                cJSON_Delete(input);
            }
        }
    }

    // Update parent IDs for kept entries
    for (int i = 0; i < h->count; i++) {
        if (i == 0) {
            h->entries[i].parent_id[0] = '\0';
        } else {
            if (h->entries[i].type != ENTRY_TOOL_RESULT) {
                strncpy(h->entries[i].parent_id, h->entries[i - 1].id, sizeof(h->entries[i].parent_id) - 1);
                h->entries[i].parent_id[sizeof(h->entries[i].parent_id) - 1] = '\0';
            }
        }
    }

    h->last_compaction_index = h->count - 1;
}

// ============================================================================
// File operation tracking
// ============================================================================

const char **history_get_read_files(History *h, int *count) {
    *count = h->read_files.count;
    return (const char **)h->read_files.paths;
}

const char **history_get_written_files(History *h, int *count) {
    *count = h->written_files.count;
    return (const char **)h->written_files.paths;
}

const char **history_get_edited_files(History *h, int *count) {
    *count = h->edited_files.count;
    return (const char **)h->edited_files.paths;
}

char *history_format_file_ops(History *h) {
    Buffer buf;
    buffer_init(&buf);

    if (h->read_files.count > 0) {
        buffer_append_str(&buf, "Read files:\n");
        for (int i = 0; i < h->read_files.count; i++) {
            buffer_append_str(&buf, "- ");
            buffer_append_str(&buf, h->read_files.paths[i]);
            buffer_append_str(&buf, "\n");
        }
    }
    if (h->written_files.count > 0) {
        buffer_append_str(&buf, "Written files:\n");
        for (int i = 0; i < h->written_files.count; i++) {
            buffer_append_str(&buf, "- ");
            buffer_append_str(&buf, h->written_files.paths[i]);
            buffer_append_str(&buf, "\n");
        }
    }
    if (h->edited_files.count > 0) {
        buffer_append_str(&buf, "Edited files:\n");
        for (int i = 0; i < h->edited_files.count; i++) {
            buffer_append_str(&buf, "- ");
            buffer_append_str(&buf, h->edited_files.paths[i]);
            buffer_append_str(&buf, "\n");
        }
    }

    if (buf.len == 0) {
        buffer_free(&buf);
        return strdup("");
    }
    return buffer_steal(&buf);
}