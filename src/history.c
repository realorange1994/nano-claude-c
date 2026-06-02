#include "config.h"
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
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    // Compute timezone offset (like Go's now.Zone())
    time_t utc = mktime(gmtime(&now));
    time_t local = mktime(t);
    int offset = (int)(local - utc);
    // mktime may adjust t for DST, recalculate
    offset = (int)((local - utc) / 60 * 60); // round to nearest hour/minute
    // Simple approach: use _timezone global on Windows
#ifdef _WIN32
    offset = -(int)_timezone;
#else
    offset = (int)(local - utc);
#endif
    int hours = offset / 3600;
    int minutes = (abs(offset) % 3600) / 60;
    char sign = offset >= 0 ? '+' : '-';
    if (hours < 0) hours = -hours;

    char inject_buf[256];
    snprintf(inject_buf, sizeof(inject_buf),
        SYSTEM_INJECTED_MARKER "[Current time: %s (UTC%c%02d:%02d)]",
        timebuf, sign, hours, minutes);

    // Remove previous time-injected entry (like Go replaces the time entry)
    for (int i = h->count - 1; i >= 0; i--) {
        if (h->entries[i].type == ENTRY_SYSTEM &&
            h->entries[i].content &&
            strstr(h->entries[i].content, "[Current time:") != NULL) {
            free(h->entries[i].content);
            // Shift entries down
            for (int j = i; j < h->count - 1; j++) {
                h->entries[j] = h->entries[j + 1];
            }
            memset(&h->entries[h->count - 1], 0, sizeof(Entry));
            h->count--;
            break;
        }
    }

    history_inject_system(h, inject_buf);
}

void history_inject_system(History *h, const char *content) {
    if (!content || !content[0]) return;
    if (h->count >= MAX_MESSAGES) return;

    Entry *e = &h->entries[h->count++];
    memset(e, 0, sizeof(Entry));
    e->type = ENTRY_SYSTEM;
    e->role = ROLE_USER;  // Injected content is sent as user message (like Go project)
    e->content = strdup(content);
    history_generate_id(e->id, sizeof(e->id));
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

    // Collect valid tool_use_ids from remaining tool calls (for orphan detection)
    int valid_tool_count = 0;
    char valid_tool_ids[2048][64];
    for (int i = 0; i < h->count && valid_tool_count < 2048; i++) {
        Entry *e = &h->entries[i];
        if (e->type == ENTRY_MESSAGE && e->role == ROLE_ASSISTANT && e->tool_name) {
            const char *tid = e->parent_id[0] ? e->parent_id : e->id;
            strncpy(valid_tool_ids[valid_tool_count], tid, 63);
            valid_tool_ids[valid_tool_count][63] = '\0';
            valid_tool_count++;
        }
    }

    int i = 0;
    while (i < h->count) {
        Entry *e = &h->entries[i];

        if (e->type == ENTRY_SYSTEM) {
            // System-injected entries become user messages with single text block
            // (like Go project's InjectTimeContext, InjectTodoReminder, etc.)
            // Group consecutive system-injected entries
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->entries[i].type == ENTRY_SYSTEM) {
                Entry *se = &h->entries[i];
                if (se->content && se->content[0]) {
                    cJSON *text_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(text_block, "type", cJSON_CreateString("text"));
                    cJSON_AddItemToObject(text_block, "text", cJSON_CreateString(se->content));
                    cJSON_AddItemToArray(content_arr, text_block);
                }
                i++;
            }

            if (cJSON_GetArraySize(content_arr) > 0) {
                cJSON_AddItemToObject(msg, "content", content_arr);
                cJSON_AddItemToArray(msgs, msg);
            } else {
                cJSON_Delete(content_arr);
                cJSON_Delete(msg);
            }
        } else if (e->type == ENTRY_TOOL_RESULT) {
            // Tool result message - user role with tool_result content blocks
            // Group consecutive tool results into one user message
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
            cJSON *content_arr = cJSON_CreateArray();

            while (i < h->count && h->entries[i].type == ENTRY_TOOL_RESULT) {
                Entry *te = &h->entries[i];
                if (te->tool_result) {
                    // Validate tool_use_id - skip orphaned results whose tool call was removed
                    const char *tid = te->parent_id[0] ? te->parent_id : NULL;
                    int is_valid = 0;
                    if (tid && tid[0]) {
                        for (int v = 0; v < valid_tool_count; v++) {
                            if (strcmp(tid, valid_tool_ids[v]) == 0) {
                                is_valid = 1;
                                break;
                            }
                        }
                    }
                    if (!is_valid) {
                        // Orphaned tool result - skip silently
                        i++;
                        continue;
                    }

                    cJSON *tool_result_block = cJSON_CreateObject();
                    cJSON_AddItemToObject(tool_result_block, "type", cJSON_CreateString("tool_result"));
                    cJSON_AddItemToObject(tool_result_block, "tool_use_id",
                        cJSON_CreateString(tid));
                    cJSON_AddItemToObject(tool_result_block, "content",
                        cJSON_CreateString(te->tool_result));
                    cJSON_AddItemToArray(content_arr, tool_result_block);
                }
                i++;
            }
            // Only add user message if it has content blocks
            if (cJSON_GetArraySize(content_arr) > 0) {
                cJSON_AddItemToObject(msg, "content", content_arr);
                cJSON_AddItemToArray(msgs, msg);
            } else {
                cJSON_Delete(content_arr);
                cJSON_Delete(msg);
            }
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

    // Log message summary for debugging
    int msg_count = cJSON_GetArraySize(msgs);
    DEBUG_LOG("[DEBUG][to_messages] %d entries -> %d messages\n", h->count, msg_count);
    for (int mi = 0; mi < msg_count && mi < 30; mi++) {
        cJSON *m = cJSON_GetArrayItem(msgs, mi);
        const char *role = cJSON_GetObjectItem(m, "role") ? cJSON_GetObjectItem(m, "role")->valuestring : "?";
        cJSON *content = cJSON_GetObjectItem(m, "content");
        if (content && content->type == cJSON_Array) {
            int blocks = cJSON_GetArraySize(content);
            DEBUG_LOG("  msg[%d]: role=%s, content_blocks=%d\n", mi, role, blocks);
            for (int bi = 0; bi < blocks && bi < 5; bi++) {
                cJSON *block = cJSON_GetArrayItem(content, bi);
                const char *btype = cJSON_GetObjectItem(block, "type") ? cJSON_GetObjectItem(block, "type")->valuestring : "?";
                const char *bname = cJSON_GetObjectItem(block, "name") ? cJSON_GetObjectItem(block, "name")->valuestring : "";
                const char *bid = cJSON_GetObjectItem(block, "tool_use_id") ? cJSON_GetObjectItem(block, "tool_use_id")->valuestring : "";
                const char *tid = cJSON_GetObjectItem(block, "id") ? cJSON_GetObjectItem(block, "id")->valuestring : "";
                DEBUG_LOG("    block[%d]: type=%s name=%s id=%s tool_use_id=%s\n", bi, btype, bname, tid, bid);
            }
        } else if (content && content->valuestring) {
            DEBUG_LOG("  msg[%d]: role=%s, text_len=%zu\n", mi, role, strlen(content->valuestring));
        } else {
            DEBUG_LOG("  msg[%d]: role=%s, (empty)\n", mi, role);
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

// Remove orphaned tool results whose tool call was cut away,
// AND orphaned tool calls whose result was cut away.
// Anthropic API requires every tool_use to have a matching tool_result and vice versa.
static void remove_orphaned_tool_results(History *h) {
    // Build set of valid tool call IDs (assistant entries with tool_name)
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

    // Build set of tool_result parent IDs (which tool_use IDs have results)
    int result_count = 0;
    char result_ids[MAX_CUT_POINTS][64];
    for (int i = 0; i < h->count && result_count < MAX_CUT_POINTS; i++) {
        Entry *e = &h->entries[i];
        if (e->type == ENTRY_TOOL_RESULT && e->parent_id[0]) {
            strncpy(result_ids[result_count], e->parent_id, 63);
            result_ids[result_count][63] = '\0';
            result_count++;
        }
    }

    DEBUG_LOG("[DEBUG][remove_orphans] entries=%d valid_tool_use=%d valid_tool_result=%d\n",
        h->count, valid_count, result_count);
    for (int i = 0; i < h->count && i < 30; i++) {
        Entry *e = &h->entries[i];
        DEBUG_LOG("  [%d] type=%d role=%d tool_name=%s parent_id=%s id=%s\n",
            i, e->type, e->role,
            e->tool_name ? e->tool_name : "(null)",
            e->parent_id[0] ? e->parent_id : "(empty)",
            e->id[0] ? e->id : "(empty)");
    }

    // Compact out orphaned entries
    int write = 0;
    int removed_results = 0, removed_uses = 0;
    for (int read = 0; read < h->count; read++) {
        Entry *e = &h->entries[read];
        if (e->type == ENTRY_TOOL_RESULT) {
            // Orphaned tool_result: no matching tool_use
            int found = 0;
            for (int v = 0; v < valid_count; v++) {
                if (strcmp(e->parent_id, valid_ids[v]) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                DEBUG_LOG("  [ORPHAN] removing tool_result idx=%d parent_id=%s\n", read, e->parent_id);
                free(e->content);
                free(e->tool_name);
                free(e->tool_result);
                removed_results++;
                continue;
            }
        } else if (e->type == ENTRY_MESSAGE && e->role == ROLE_ASSISTANT && e->tool_name && e->tool_name[0]) {
            // Orphaned tool_use: no matching tool_result
            const char *tid = e->parent_id[0] ? e->parent_id : e->id;
            int found = 0;
            for (int v = 0; v < result_count; v++) {
                if (strcmp(tid, result_ids[v]) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                DEBUG_LOG("  [ORPHAN] removing tool_use idx=%d tool=%s tid=%s\n", read, e->tool_name, tid);
                free(e->content);
                free(e->tool_name);
                free(e->tool_result);
                removed_uses++;
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

    DEBUG_LOG("[DEBUG][remove_orphans] removed %d tool_results, %d tool_uses, remaining=%d\n",
        removed_results, removed_uses, h->count);
}

void history_compact(History *h, char *(*summarize_fn)(const char *)) {
    if (!h) { DEBUG_LOG("[DEBUG][compact] history is NULL\n"); return; }
    if (h->count == 0) { DEBUG_LOG("[DEBUG][compact] history count is 0\n"); return; }

    DEBUG_LOG("[DEBUG][compact] Starting compaction, entries=%d\n", h->count);

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

    DEBUG_LOG("[DEBUG][compact] Built old_content, len=%zu\n", buffer_len(&old_content));

    char *summary = (*summarize_fn)(buffer_c_str(&old_content));
    buffer_free(&old_content);

    DEBUG_LOG("[DEBUG][compact] summarize_fn returned: %s\n", summary ? "summary" : "NULL");

    if (!summary || !summary[0]) {
        free(summary);
        fprintf(stderr, "[Compaction failed: no summary generated]\n");
        return;
    }

    // Store the summary BEFORE replacing entries
    free(h->summary);
    h->summary = summary;

    DEBUG_LOG("[DEBUG][compact] Summary stored, len=%zu\n", strlen(summary));

    // Find cut point
    int cut_index = history_find_cut_point(h);
    DEBUG_LOG("[DEBUG][compact] cut_index=%d count=%d\n", cut_index, h->count);

    if (cut_index >= h->count) cut_index = h->count - 1;
    if (cut_index == 0) { DEBUG_LOG("[DEBUG][compact] cut_index==0, nothing to compact\n"); return; }

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

    // Update parent IDs for kept entries (only for non-tool entries)
    // DO NOT overwrite tool_use parent_id - it stores the original tool call ID from API
    // DO NOT overwrite tool_result parent_id - it stores the tool_use_id it responds to
    for (int i = 0; i < h->count; i++) {
        Entry *e = &h->entries[i];
        // Skip tool_use entries (assistant with tool_name) - they need their original tool call ID
        if (e->type == ENTRY_MESSAGE && e->role == ROLE_ASSISTANT && e->tool_name && e->tool_name[0]) {
            continue;
        }
        // Skip tool_result entries - they need their original tool_use_id
        if (e->type == ENTRY_TOOL_RESULT) {
            continue;
        }
        // Only update parent_id for regular messages (link to previous entry)
        if (i == 0) {
            e->parent_id[0] = '\0';
        } else {
            strncpy(e->parent_id, h->entries[i - 1].id, sizeof(e->parent_id) - 1);
            e->parent_id[sizeof(e->parent_id) - 1] = '\0';
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