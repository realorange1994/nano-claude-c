#include "repl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>

// ============================================================================
// Global Ctrl+C state (like miniclaude's windowsOnInterrupt + lastCtrlC)
// ============================================================================

static REPL *g_repl = NULL;  // Global REPL pointer for Ctrl+C handler

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        if (!g_repl) return FALSE;

        DWORD now = GetTickCount();
        DWORD last = g_repl->last_interrupt_time;

        // Double Ctrl+C within 1.5s → exit immediately (like miniclaude)
        if (last > 0 && now - last < 1500) {
            printf("\nExiting...\n");
            fflush(stdout);
            ExitProcess(0);
        }

        g_repl->last_interrupt_time = now;
        g_repl->cancelled = 1;

        printf("\n[Interrupted. Press Ctrl+C again within 1.5s to exit.]\n");
        fflush(stdout);

        // Flush console input buffer to clear any pending Ctrl+C events
        // This helps prevent issues with ReadConsoleA after Ctrl+C
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD ir;
        DWORD numRead;
        while (PeekConsoleInput(hStdin, &ir, 1, &numRead) && numRead > 0) {
            ReadConsoleInput(hStdin, &ir, 1, &numRead);  // Discard event
        }

        return TRUE;  // Suppress default handler (prevents process kill)
    }
    return FALSE;
}

// Ensure console input mode is correct (like miniclaude's ensureConsoleInputMode)
// MCP child processes may alter the console mode.
static void ensure_console_input_mode(void) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;

    DWORD mode;
    if (!GetConsoleMode(h, &mode)) return;

    DWORD need = mode;
    need |= ENABLE_PROCESSED_INPUT;
    need |= ENABLE_LINE_INPUT;
    need |= ENABLE_ECHO_INPUT;
    need &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
    need |= ENABLE_EXTENDED_FLAGS;

    if (need != mode) {
        SetConsoleMode(h, need);
    }
}

// ============================================================================
// REPL lifecycle
// ============================================================================

REPL *repl_new(Provider *provider, ToolRegistry *tools) {
    REPL *repl = calloc(1, sizeof(REPL));
    if (!repl) return NULL;

    repl->provider = provider;
    repl->tools = tools;
    history_init(&repl->history, 16384, 20000);
    repl->running = true;

    return repl;
}

void repl_free(REPL *repl) {
    if (!repl) return;

    repl_clear_tools(repl);

    for (int i = 0; i < repl->mcp_count; i++) {
        if (repl->mcp_clients[i]) {
            mcp_client_free(repl->mcp_clients[i]);
        }
    }

    history_free(&repl->history);
    free(repl);
}

// ============================================================================
// Pending tool call management
// ============================================================================

void repl_clear_tools(REPL *repl) {
    for (int i = 0; i < repl->pending_tool_count; i++) {
        free(repl->pending_tool_names[i]);
        free(repl->pending_tool_ids[i]);
        free(repl->pending_tool_inputs[i]);
    }
    repl->pending_tool_count = 0;
}

void repl_add_tool(REPL *repl, const char *name, const char *id, const char *input) {
    if (repl->pending_tool_count >= MAX_PENDING_TOOLS) return;
    int i = repl->pending_tool_count++;
    repl->pending_tool_names[i] = name ? strdup(name) : NULL;
    repl->pending_tool_ids[i] = id ? strdup(id) : NULL;
    repl->pending_tool_inputs[i] = input ? strdup(input) : strdup("{}");
}

// ============================================================================
// Cancellation context (like miniclaude's context.WithCancel)
// ============================================================================

void repl_cancel(REPL *repl) {
    if (repl) repl->cancelled = 1;
}

void repl_reset_cancel(REPL *repl) {
    if (repl) repl->cancelled = 0;
}

bool repl_is_cancelled(REPL *repl) {
    return repl && repl->cancelled;
}

void repl_console_ctrl_handler(REPL *repl) {
    // Called from SetConsoleCtrlHandler - just set the flag
    if (repl) repl->cancelled = 1;
}

// ============================================================================
// Interruptible input reading (like miniclaude's readLineInterruptible)
// Uses WaitForSingleObject to poll stdin with 100ms timeout,
// allowing Ctrl+C cancellation to interrupt cleanly.
// ============================================================================

char *repl_read_line_interruptible(REPL *repl) {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char line[4096];
    int pos = 0;
    char ch;

    // Restore console mode before reading
    ensure_console_input_mode();

    while (1) {
        // Check cancellation
        if (repl_is_cancelled(repl)) {
            return NULL;
        }

        // Wait for input with 100ms timeout
        DWORD waitResult = WaitForSingleObject(hStdin, 100);

        switch (waitResult) {
        case WAIT_OBJECT_0: {
            DWORD charsRead;
            if (!ReadConsoleA(hStdin, &ch, 1, &charsRead, NULL)) {
                // ReadConsoleA failed - check cancellation and console state
                if (repl_is_cancelled(repl)) {
                    return NULL;
                }
                // Restore console mode and retry
                ensure_console_input_mode();
                Sleep(50);
                continue;
            }
            if (charsRead == 0) {
                // No character read - could be Ctrl+C or console event
                // Check cancellation first
                if (repl_is_cancelled(repl)) {
                    return NULL;
                }
                // Restore console mode and retry
                ensure_console_input_mode();
                Sleep(50);
                continue;
            }
            if (ch == '\n') {
                line[pos] = '\0';
                return strdup(line);
            }
            if (ch == '\r') {
                continue;
            }
            if (pos < (int)sizeof(line) - 1) {
                line[pos++] = ch;
            }
            break;
        }
        case WAIT_TIMEOUT:
            continue;
        case WAIT_FAILED:
        default: {
            // Fallback: use fgets for non-console handles
            if (fgets(line, sizeof(line), stdin) == NULL) {
                return NULL;
            }
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            return strdup(line);
        }
        }
    }
}

// ============================================================================
// MCP
// ============================================================================

bool repl_add_mcp(REPL *repl, const char *name, const char *command) {
    if (repl->mcp_count >= MAX_MCP_CLIENTS) return false;

    MCPClient *mcp = mcp_client_new(name, command);
    if (!mcp) return false;

    if (!mcp_initialize(mcp)) {
        mcp_client_free(mcp);
        return false;
    }

    cJSON *tools = mcp_list_tools(mcp);
    if (tools) {
        cJSON *tool_arr = cJSON_GetObjectItem(tools, "tools");
        if (tool_arr) {
            int count = cJSON_GetArraySize(tool_arr);
            for (int i = 0; i < count; i++) {
                cJSON *t = cJSON_GetArrayItem(tool_arr, i);
                cJSON *tname = cJSON_GetObjectItem(t, "name");
                if (tname && tname->valuestring) {
                    fprintf(stderr, "[DEBUG] Found MCP tool: %s\n", tname->valuestring);
                }
            }
        }
        cJSON_Delete(tools);
    }

    repl->mcp_clients[repl->mcp_count++] = mcp;
    return true;
}

// ============================================================================
// Streaming & text accumulation
// ============================================================================

static void repl_accum_append(REPL *repl, const char *text) {
    size_t len = strlen(text);
    if (len == 0) return;
    size_t new_len = repl->text_len + len + 1;
    if (!repl->text_accum) {
        repl->text_accum = malloc(new_len);
    } else {
        repl->text_accum = realloc(repl->text_accum, new_len);
    }
    if (repl->text_accum) {
        memcpy(repl->text_accum + repl->text_len, text, len);
        repl->text_len += len;
        repl->text_accum[repl->text_len] = '\0';
    }
}

static void repl_accum_reset(REPL *repl) {
    free(repl->text_accum);
    repl->text_accum = NULL;
    repl->text_len = 0;
    repl_clear_tools(repl);
}

static void stream_callback(const StreamChunk *chunk, void *userdata) {
    REPL *repl = (REPL*)userdata;

    // Check cancellation during streaming (like miniclaude's ctx.Done() check)
    if (repl_is_cancelled(repl)) return;

    switch (chunk->type) {
        case CHUNK_CONTENT:
            if (chunk->content) {
                printf("%s", chunk->content);
                fflush(stdout);
                repl_accum_append(repl, chunk->content);
            }
            break;
        case CHUNK_TOOL_CALL:
            if (chunk->tool_name && chunk->tool_id) {
                repl_add_tool(repl, chunk->tool_name, chunk->tool_id,
                             chunk->tool_input ? chunk->tool_input : "{}");
            }
            break;
        case CHUNK_DONE:
            printf("\n");
            fflush(stdout);
            break;
        case CHUNK_ERROR:
            if (chunk->error) {
                fprintf(stderr, "Error: %s\n", chunk->error);
            }
            break;
    }
}

// ============================================================================
// Compaction (like miniclaude's runCompaction)
// ============================================================================

static const char *SUMMARIZATION_PROMPT =
"The messages above are a conversation to summarize. Create a structured context checkpoint summary.\n\n"
"## Goal\n[What is the user trying to accomplish?]\n\n"
"## Progress\n- [x] [Completed tasks]\n- [ ] [Current work]\n\n"
"## Next Steps\n1. [Ordered list]\n\n"
"## Critical Context\n- [Data, references]\n";

static char *truncate_for_summary(const char *content, int max_len) {
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

static char *build_history_text(REPL *repl) {
    char *buf = malloc(65536);
    if (!buf) return NULL;
    buf[0] = '\0';
    int pos = 0;
    int cap = 65536;

    for (int i = 0; i < repl->history.count; i++) {
        Message *m = &repl->history.msgs[i];
        const char *role = m->role ? m->role : "unknown";
        char *truncated = NULL;

        if (m->type == MSG_ASSISTANT && m->tool_name) {
            truncated = truncate_for_summary(m->content ? m->content : "{}", 500);
            pos += snprintf(buf + pos, cap - pos, "[ASSISTANT - Tool Call: %s]: %s\n",
                           m->tool_name, truncated);
        } else if (m->type == MSG_TOOL) {
            truncated = truncate_for_summary(m->tool_result ? m->tool_result : "", 500);
            pos += snprintf(buf + pos, cap - pos, "[TOOL RESULT - %s]: %s\n",
                           m->tool_name ? m->tool_name : "unknown", truncated);
        } else {
            truncated = truncate_for_summary(m->content ? m->content : "", 1000);
            pos += snprintf(buf + pos, cap - pos, "[%s]: %s\n", role, truncated);
        }
        free(truncated);
        if (pos >= cap - 100) break;
    }
    return buf;
}

static void repl_run_compaction(REPL *repl) {
    char *history_text = build_history_text(repl);
    if (!history_text || !history_text[0]) {
        free(history_text);
        return;
    }

    char *prompt = malloc(strlen(history_text) + strlen(SUMMARIZATION_PROMPT) + 100);
    if (!prompt) { free(history_text); return; }
    sprintf(prompt, "%s\n\n%s", SUMMARIZATION_PROMPT, history_text);
    free(history_text);

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
    cJSON_AddItemToObject(msg, "content", cJSON_CreateString(prompt));
    cJSON_AddItemToArray(messages, msg);
    free(prompt);

    char *summary = provider_chat_sync(repl->provider, messages);
    cJSON_Delete(messages);

    if (summary && summary[0]) {
        int keep = 4;
        if (repl->history.count > keep) {
            for (int i = 0; i < repl->history.count - keep; i++) {
                Message *m = &repl->history.msgs[i];
                free(m->content); free(m->tool_id); free(m->tool_name); free(m->tool_result);
            }
            int shift = repl->history.count - keep;
            for (int i = 0; i < keep; i++) {
                repl->history.msgs[i] = repl->history.msgs[i + shift];
            }
            for (int i = keep; i < repl->history.count; i++) {
                memset(&repl->history.msgs[i], 0, sizeof(Message));
            }
            repl->history.count = keep;
        }

        // Remove orphaned tool results (like miniclaude's removeOrphanedToolResults)
        int valid_count = 0;
        char valid_ids[MAX_MESSAGES][256];
        for (int i = 0; i < repl->history.count && valid_count < MAX_MESSAGES; i++) {
            if (repl->history.msgs[i].type == MSG_ASSISTANT && repl->history.msgs[i].tool_id) {
                strncpy(valid_ids[valid_count], repl->history.msgs[i].tool_id, 255);
                valid_ids[valid_count][255] = '\0';
                valid_count++;
            }
        }
        int new_count = 0;
        for (int i = 0; i < repl->history.count; i++) {
            Message *m = &repl->history.msgs[i];
            if (m->type == MSG_TOOL) {
                int found = 0;
                if (m->tool_id) {
                    for (int j = 0; j < valid_count; j++) {
                        if (strcmp(m->tool_id, valid_ids[j]) == 0) { found = 1; break; }
                    }
                }
                if (!found) {
                    free(m->content); free(m->tool_id); free(m->tool_name); free(m->tool_result);
                    memset(m, 0, sizeof(Message));
                    continue;
                }
            }
            if (new_count != i) {
                repl->history.msgs[new_count] = *m;
                memset(m, 0, sizeof(Message));
            }
            new_count++;
        }
        repl->history.count = new_count;

        free(repl->history.summary);
        repl->history.summary = summary;
        printf("[History summarized. Continuing...]\n");
    } else {
        free(summary);
        printf("[Compaction failed: no summary generated]\n");
    }
}

// ============================================================================
// Time context injection (like miniclaude's InjectTimeContext)
// ============================================================================

static void history_inject_time(History *h) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), "[Current time: %Y-%m-%d %H:%M:%S]", t);

    for (int i = h->count - 1; i >= 0; i--) {
        if (h->msgs[i].type == MSG_USER && h->msgs[i].content &&
            strncmp(h->msgs[i].content, "[Current time:", 14) == 0) {
            free(h->msgs[i].content);
            h->msgs[i].content = strdup(timebuf);
            return;
        }
    }
    history_add(h, MSG_USER, timebuf);
}

// ============================================================================
// Tool action display (like miniclaude's FormatAction)
// ============================================================================

static const char *cjson_get_str(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && v->valuestring) ? v->valuestring : "";
}

static void print_tool_action(const char *name, cJSON *input) {
    if (strcmp(name, "Shell") == 0) {
        printf("Running: %.120s", cjson_get_str(input, "command"));
    } else if (strcmp(name, "Read") == 0) {
        const char *path = cjson_get_str(input, "path");
        const char *offset = cjson_get_str(input, "offset");
        if (offset[0] && offset[0] != '0')
            printf("Reading: %s (line %s)", path, offset);
        else
            printf("Reading: %s", path);
    } else if (strcmp(name, "Write") == 0) {
        printf("Writing: %s", cjson_get_str(input, "path"));
    } else if (strcmp(name, "Edit") == 0) {
        printf("Editing: %s", cjson_get_str(input, "path"));
    } else if (strcmp(name, "Grep") == 0) {
        const char *pattern = cjson_get_str(input, "pattern");
        const char *path = cjson_get_str(input, "path");
        if (path[0])
            printf("Searching \"%.60s\" in %s", pattern, path);
        else
            printf("Searching \"%.60s\"", pattern);
    } else if (strcmp(name, "Glob") == 0) {
        printf("Finding: %s", cjson_get_str(input, "pattern"));
    } else if (strcmp(name, "Calc") == 0) {
        printf("Calculating: %.80s", cjson_get_str(input, "expr"));
    } else {
        printf("[%s]", name);
    }
    printf("\n");
}

// ============================================================================
// Main REPL loop (like miniclaude's REPL.Run + runLoop)
// ============================================================================

int repl_run(REPL *repl) {
    // Install Ctrl+C handler (like miniclaude's SetConsoleCtrlHandler)
    g_repl = repl;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    // Check if stdin is a terminal
    const char *test_mode = getenv("NANOCLAUDE_TEST");
    if (!test_mode) {
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        DWORD type = GetFileType(h);
        if (type != FILE_TYPE_CHAR) {
            fprintf(stderr, "Error: stdin is not a terminal. This program requires interactive input.\n");
            fprintf(stderr, "Run without pipes or redirects.\n");
            g_repl = NULL;
            SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
            return 1;
        }
    }

    printf("NanoClaude-C (Pure C AI Agent)\n");
    printf("Type '/help' for commands, '/quit' to exit, Ctrl+C to interrupt\n\n");
    fflush(stdout);

    while (repl->running) {
        // Restore console input mode (MCP processes may alter it)
        ensure_console_input_mode();

        // Reset cancellation for new input
        repl_reset_cancel(repl);

        printf("\n> ");
        fflush(stdout);

        // Read input with Ctrl+C support (like miniclaude's readLineInterruptible)
        char *line = repl_read_line_interruptible(repl);
        if (!line) {
            // Ctrl+C was pressed during input - just continue
            if (repl_is_cancelled(repl)) {
                continue;
            }
            // EOF
            break;
        }

        // Handle commands
        if (line[0] == '/') {
            if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
                free(line);
                break;
            } else if (strcmp(line, "/help") == 0) {
                printf("Commands:\n");
                printf("  /quit, /exit  - Exit the program\n");
                printf("  /help         - Show this help\n");
                printf("  /clear        - Clear history\n");
                free(line);
                continue;
            } else if (strcmp(line, "/clear") == 0) {
                history_free(&repl->history);
                history_init(&repl->history, 16384, 20000);
                printf("History cleared.\n");
                free(line);
                continue;
            } else {
                printf("Unknown command: %s\n", line);
                free(line);
                continue;
            }
        }

        // Skip empty input
        if (strlen(line) == 0) {
            free(line);
            continue;
        }

        // Add user message to history
        history_add(&repl->history, MSG_USER, line);
        free(line);

        // Agent loop (like miniclaude's runLoop)
        int iteration = 0;
        for (iteration = 0; iteration < 100; iteration++) {
            // Reset cancellation for new API call
            repl_reset_cancel(repl);

            // Reset streaming accumulator
            repl_accum_reset(repl);

            // Inject current time (like miniclaude's InjectTimeContext)
            history_inject_time(&repl->history);

            // Get messages for API
            cJSON *messages = history_to_messages(&repl->history);

            // Stream chat
            bool success = provider_chat_stream(repl->provider, messages, stream_callback, repl, repl->tools, &repl->cancelled);
            cJSON_Delete(messages);

            // Check if interrupted during streaming
            if (repl_is_cancelled(repl)) {
                printf("\n[Interrupted.]\n");
                break;
            }

            if (!success) {
                printf("Error: Failed to get response\n");
                break;
            }

            int tool_count = repl->pending_tool_count;

            // Save reasoning text even when tool calls present (like miniclaude)
            if (repl->text_accum && repl->text_len > 0) {
                history_add(&repl->history, MSG_ASSISTANT, repl->text_accum);
            }

            // If no tool calls, check compaction and done
            if (tool_count == 0) {
                if (history_needs_compact(&repl->history)) {
                    printf("\n[Context limit approaching, summarizing history...]\n");
                    repl_run_compaction(repl);
                }
                break;
            }

            // Record tool calls BEFORE executing (like miniclaude's AddToolCall)
            for (int i = 0; i < tool_count; i++) {
                history_add_tool_call(&repl->history,
                                     repl->pending_tool_ids[i],
                                     repl->pending_tool_names[i],
                                     repl->pending_tool_inputs[i]);
            }

            // Execute each tool (like miniclaude's loop over toolCalls)
            bool interrupted = false;
            for (int i = 0; i < tool_count; i++) {
                // Check cancellation before each tool (like miniclaude's toolCtx)
                if (repl_is_cancelled(repl)) {
                    printf("\n[Interrupted during tool execution.]\n");
                    interrupted = true;
                    break;
                }

                cJSON *args = repl->pending_tool_inputs[i]
                    ? cJSON_Parse(repl->pending_tool_inputs[i])
                    : NULL;

                print_tool_action(repl->pending_tool_names[i], args);

                char *error = NULL;
                char *result = tool_execute(repl->tools, repl->pending_tool_names[i], args, &error);

                if (result) {
                    history_add_tool(&repl->history, repl->pending_tool_ids[i],
                                    repl->pending_tool_names[i], result);
                    printf("%s\n", result);
                    free(result);
                } else if (error) {
                    history_add_tool(&repl->history, repl->pending_tool_ids[i],
                                    repl->pending_tool_names[i], error);
                    printf("Error: %s\n", error);
                    free(error);
                }

                if (args) cJSON_Delete(args);
            }

            if (interrupted) break;

            // Check compaction
            if (history_needs_compact(&repl->history)) {
                printf("\n[Context limit approaching, summarizing history...]\n");
                repl_run_compaction(repl);
            }

            // Clear pending tools and continue to next iteration
            repl_clear_tools(repl);
            continue;
        }

        if (iteration >= 100) {
            printf("\n[Warning: Max iterations (100) reached]\n");
        }
    }

    // Remove Ctrl+C handler
    g_repl = NULL;
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);

    return 0;
}

// ============================================================================
// Utility functions
// ============================================================================

char *repl_read_line(const char *prompt) {
    char buf[4096];
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) == NULL) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return strdup(buf);
}

void repl_print(const char *str) { printf("%s", str); }
void repl_print_error(const char *str) { fprintf(stderr, "%s", str); }
