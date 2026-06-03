#include "repl.h"
#include "config.h"
#include "system_prompt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>
#endif

// ============================================================================
// Global Ctrl+C state (like miniclaude's windowsOnInterrupt + lastCtrlC)
// ============================================================================

static REPL *g_repl = NULL;  // Global REPL pointer for Ctrl+C handler

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        if (!g_repl) return FALSE;

        DWORD now = GetTickCount();
        DWORD last = g_repl->last_interrupt_time;

        // Double Ctrl+C within 1.5s -> exit immediately (like miniclaude)
        if (last > 0 && now - last < 1500) {
            printf("\nExiting...\n");
            fflush(stdout);
            ExitProcess(0);
        }

        g_repl->last_interrupt_time = now;
        g_repl->cancelled = 1;
        extern volatile LONG g_interrupted;
        extern int g_in_tool;
        g_interrupted = 1;

        printf("\n[Interrupted. Press Ctrl+C again within 1.5s to exit.]\n");
        fflush(stdout);

        // Flush console input buffer to clear any pending Ctrl+C events
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
#else
static unsigned long get_ticks_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void console_signal_handler(int sig) {
    if (sig == SIGINT) {
        if (!g_repl) return;

        unsigned long now = get_ticks_ms();
        unsigned long last = g_repl->last_interrupt_time;

        // Double Ctrl+C within 1.5s -> exit immediately
        if (last > 0 && now - last < 1500) {
            printf("\nExiting...\n");
            fflush(stdout);
            _exit(130);
        }

        g_repl->last_interrupt_time = now;
        g_repl->cancelled = 1;
        extern volatile int g_interrupted;
        g_interrupted = 1;

        printf("\n[Interrupted. Press Ctrl+C again within 1.5s to exit.]\n");
        fflush(stdout);
    }
}
#endif

// Ensure console input mode is correct (like miniclaude's ensureConsoleInputMode)
// MCP child processes may alter the console mode.
static void ensure_console_input_mode(void) {
#ifdef _WIN32
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
#else
    // Linux: nothing to do - terminal handles line input by default
    (void)0;
#endif
}

// ============================================================================
// REPL lifecycle
// ============================================================================

REPL *repl_new(Provider *provider, ToolRegistry *tools) {
    REPL *repl = calloc(1, sizeof(REPL));
    if (!repl) return NULL;

    repl->provider = provider;
    repl->tools = tools;
    history_init(&repl->history, g_config.reserve_tokens, g_config.keep_recent_tokens, g_config.context_window);
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


// ============================================================================
// Interruptible input reading (like miniclaude's readLineInterruptible)
// Uses WaitForSingleObject to poll stdin with 100ms timeout,
// allowing Ctrl+C cancellation to interrupt cleanly.
// ============================================================================

char *repl_read_line_interruptible(REPL *repl) {
#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char line[4096];
    int pos = 0;
    char ch;

    // Check if stdin is a pipe (not a console)
    DWORD file_type = GetFileType(hStdin);
    int is_pipe = (file_type == FILE_TYPE_CHAR) ? 0 : 1;
    if (is_pipe) {
        // Use fgets for piped/non-console input
        static char pipe_line[4096];
        if (fgets(pipe_line, sizeof(pipe_line), stdin) == NULL) return NULL;
        size_t len = strlen(pipe_line);
        if (len > 0 && pipe_line[len - 1] == '\n') pipe_line[--len] = '\0';
        if (len > 0 && pipe_line[len - 1] == '\r') pipe_line[--len] = '\0';
        return strdup(pipe_line);
    }

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
                if (repl_is_cancelled(repl)) return NULL;
                ensure_console_input_mode();
                Sleep(50);
                continue;
            }
            if (charsRead == 0) {
                if (repl_is_cancelled(repl)) return NULL;
                ensure_console_input_mode();
                Sleep(50);
                continue;
            }
            if (ch == '\n') {
                line[pos] = '\0';
                return strdup(line);
            }
            if (ch == '\r') continue;
            if (pos < (int)sizeof(line) - 1) line[pos++] = ch;
            break;
        }
        case WAIT_TIMEOUT:
            continue;
        case WAIT_FAILED:
        default: {
            // Fallback: use fgets for non-console handles
            if (fgets(line, sizeof(line), stdin) == NULL) return NULL;
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            return strdup(line);
        }
        }
    }
#else
    // Linux: simply use fgets with signal-based cancellation
    static char line[4096];
    if (repl_is_cancelled(repl)) return NULL;

    if (fgets(line, sizeof(line), stdin) == NULL) {
        return NULL;  // EOF
    }

    if (repl_is_cancelled(repl)) return NULL;

    // Strip newline
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
    if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
    return strdup(line);
#endif
}

// ============================================================================
// MCP
// ============================================================================

bool repl_add_mcp(REPL *repl, ToolRegistry *tools, const char *name, const char *command) {
    if (repl->mcp_count >= MAX_MCP_CLIENTS) return false;

    MCPClient *mcp = mcp_client_new(name, command);
    if (!mcp) return false;

    if (!mcp_initialize(mcp)) {
        mcp_client_free(mcp);
        return false;
    }

    cJSON *mcp_tools = mcp_list_tools(mcp);
    int registered = 0;

    if (mcp_tools) {
        // mcp_list_tools returns the tools array directly (not wrapped in {"tools": [...]})
        int count = cJSON_GetArraySize(mcp_tools);

        for (int i = 0; i < count; i++) {
            cJSON *t = cJSON_GetArrayItem(mcp_tools, i);
            cJSON *tname = cJSON_GetObjectItem(t, "name");

            if (tname && tname->valuestring) {
                cJSON *tdesc = cJSON_GetObjectItem(t, "description");
                cJSON *ischema = cJSON_GetObjectItem(t, "input_schema");

                char *tool_name = strdup(tname->valuestring);
                char *tool_desc = tdesc ? strdup(tdesc->valuestring) : strdup("MCP tool");
                cJSON *schema_copy = ischema ? cJSON_Duplicate(ischema, 1) : cJSON_CreateObject();

                MCPToolBinding *binding = calloc(1, sizeof(MCPToolBinding));
                if (binding) {
                    binding->mcp = mcp;
                    binding->tool_name = strdup(tname->valuestring);

                    if (tools) {
                        if (!tool_registry_register_mcp(tools, tool_name, tool_desc, schema_copy, NULL, binding)) {
                            free(tool_name);
                            free(tool_desc);
                            cJSON_Delete(schema_copy);
                            free(binding->tool_name);
                            free(binding);
                        } else {
                            registered++;
                        }
                    }
                }
            }
        }

        cJSON_Delete(mcp_tools);
        fprintf(stderr, "[MCP] registered %d tools from %s\n", registered, name);
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
        if (!repl->text_accum) return;
        repl->text_cap = new_len;
    } else if (new_len > repl->text_cap) {
        char *new_buf = realloc(repl->text_accum, new_len * 2);
        if (!new_buf) {
            return;
        }
        repl->text_accum = new_buf;
        repl->text_cap = new_len * 2;
    }
    memcpy(repl->text_accum + repl->text_len, text, len);
    repl->text_len += len;
    repl->text_accum[repl->text_len] = '\0';
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
    }
}

// ============================================================================
// Compaction (like miniclaude's runCompaction)
// ============================================================================

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int done;
} SummaryCollector;

static void collect_summary_chunk(const StreamChunk *chunk, void *userdata) {
    SummaryCollector *c = (SummaryCollector *)userdata;
    if (chunk->type == CHUNK_CONTENT && chunk->content) {
        size_t clen = strlen(chunk->content);
        if (c->len + clen + 1 > c->cap) {
            c->cap = (c->len + clen + 1) * 2;
            char *new_buf = realloc(c->buf, c->cap);
            if (!new_buf) return;
            c->buf = new_buf;
        }
        memcpy(c->buf + c->len, chunk->content, clen);
        c->len += clen;
        c->buf[c->len] = '\0';
    } else if (chunk->type == CHUNK_DONE) {
        c->done = 1;
    }
}

static char *repl_summarize_callback(const char *history_text) {
    DEBUG_LOG("[DEBUG][summarize] Starting summarization, text_len=%zu\n", history_text ? strlen(history_text) : 0);

    const char *prompt_template = history_get_summarization_prompt();
    char *prompt = malloc(strlen(history_text) + strlen(prompt_template) + 200);
    if (!prompt) {
    DEBUG_LOG("[DEBUG][summarize] malloc failed for prompt\n");
        return NULL;
    }
    sprintf(prompt, "%s\n\n%s", prompt_template, history_text);

    cJSON *messages = cJSON_CreateArray();
    if (!messages) { free(prompt); DEBUG_LOG("[DEBUG][summarize] cJSON_CreateArray failed\n"); return NULL; }
    cJSON *msg = cJSON_CreateObject();
    if (!msg) { cJSON_Delete(messages); free(prompt); DEBUG_LOG("[DEBUG][summarize] cJSON_CreateObject failed\n"); return NULL; }
    cJSON_AddItemToObject(msg, "role", cJSON_CreateString("user"));
    cJSON_AddItemToObject(msg, "content", cJSON_CreateString(prompt));
    cJSON_AddItemToArray(messages, msg);
    free(prompt);

    // Use streaming (like miniclaude) - sync mode is rejected by some proxies
    SummaryCollector sc = { NULL, 0, 0, 0 };
    long not_cancelled = 0;
    bool success = provider_chat_stream(g_repl->provider, messages, collect_summary_chunk, &sc, NULL, &not_cancelled, NULL);
    cJSON_Delete(messages);

    char *summary = NULL;
    if (success && sc.done && sc.buf && sc.len > 0) {
        summary = sc.buf;
    } else {
        free(sc.buf);
        summary = NULL;
    }

    DEBUG_LOG("[DEBUG][summarize] Result: %s (len=%zu)\n",
        summary ? "got summary" : "NULL", summary ? strlen(summary) : 0);

    return summary;
}

static void repl_run_compaction(REPL *repl) {
    if (!repl) return;

    // history_compact handles everything: build content for summarization,
    // store summary, cut entries, remove orphaned tool results,
    // rebuild file operations, update parent IDs
    history_compact(&repl->history, repl_summarize_callback);

    // Report the new token count
    int tokens = history_estimate_tokens(&repl->history);
    printf("[Compaction complete. ~%d tokens remaining]\n", tokens);
}

// ============================================================================
// Tool action display (like miniclaude's FormatAction)
// ============================================================================

static const char *cjson_get_str(cJSON *obj, const char *key) {
    if (!obj) return "";
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && v->valuestring) ? v->valuestring : "";
}

static void print_tool_action(const char *name, cJSON *input) {
    if (strcmp(name, "Shell") == 0) {
        printf("Running: %s", cjson_get_str(input, "command"));
    } else if (strcmp(name, "Read") == 0) {
        const char *path = cjson_get_str(input, "file_path");
        const char *offset = cjson_get_str(input, "offset");
        if (offset && offset[0] && offset[0] != '0')
            printf("Reading: %s (line %s)", path, offset);
        else
            printf("Reading: %s", path);
    } else if (strcmp(name, "Write") == 0) {
        printf("Writing: %s", cjson_get_str(input, "file_path"));
    } else if (strcmp(name, "Edit") == 0) {
        printf("Editing: %s", cjson_get_str(input, "file_path"));
    } else if (strcmp(name, "Grep") == 0) {
        const char *pattern = cjson_get_str(input, "pattern");
        const char *path = cjson_get_str(input, "path");
        if (path && path[0])
            printf("Searching \"%.60s\" in %s", pattern, path);
        else
            printf("Searching \"%.60s\"", pattern);
    } else if (strcmp(name, "Glob") == 0) {
        printf("Finding: %s", cjson_get_str(input, "pattern"));
    } else if (strcmp(name, "Calc") == 0) {
        printf("Calculating: %s", cjson_get_str(input, "expr"));
    } else {
        // MCP or unknown tools - print with their input params
        printf("Calling: %s", name);
        if (input) {
            char *json = cJSON_PrintUnformatted(input);
            if (json && json[0]) {
                // Truncate long JSON for display
                size_t jlen = strlen(json);
                if (jlen > 200) jlen = 200;
                printf(" (%.*s%s)", (int)jlen, json, (jlen != strlen(json)) ? "..." : "");
            }
            free(json);
        }
    }

    printf("\n");
}

// ============================================================================
// Main REPL loop (like miniclaude's REPL.Run + runLoop)
// ============================================================================

int repl_run(REPL *repl) {
    // Install Ctrl+C handler (like miniclaude's SetConsoleCtrlHandler)
    g_repl = repl;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    signal(SIGINT, console_signal_handler);
#endif

    // Build system prompt once (reuse across all iterations)
    const char *model_name = provider_model(repl->provider);
    char *system_prompt = system_prompt_build(repl->tools, model_name, "auto", NULL);

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
                history_init(&repl->history, g_config.reserve_tokens, g_config.keep_recent_tokens, g_config.context_window);
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
        history_add_user(&repl->history, line);
        free(line);

        // Agent loop (like miniclaude's runLoop)
        int iteration = 0;
        for (iteration = 0; iteration < 100; iteration++) {
            // Reset cancellation for new API call
            repl_reset_cancel(repl);

            // Reset streaming accumulator
            repl_accum_reset(repl);

            // Reset retry state for new API call
            provider_reset_retry_state(repl->provider);

            // Increment turn counter for idle/todo tracking
            todo_list_increment_turn();

            // Inject current time (like miniclaude's InjectTimeContext)
            history_inject_time_context(&repl->history);

            // Inject todo reminder if there's a todo list
            {
                char *reminder = todo_list_build_reminder();
                if (reminder) {
                    history_inject_system(&repl->history, reminder);
                    free(reminder);
                } else {
                    // Idle reminder: if model hasn't used TodoWrite for a while
                    char *idle = todo_list_build_idle_reminder();
                    if (idle) {
                        history_inject_system(&repl->history, idle);
                        free(idle);
                    }
                }
            }

            // Get messages for API
            cJSON *messages = history_to_messages(&repl->history);

            // Stream chat
            bool success = provider_chat_stream(repl->provider, messages, stream_callback, repl, repl->tools, &repl->cancelled, system_prompt);
            cJSON_Delete(messages);

            // Check if interrupted during streaming
            if (repl_is_cancelled(repl)) {
                printf("\n[Interrupted.]\n");
                break;
            }

            if (!success) {
                int http_status;
                char error_msg[512];
                provider_get_last_error(repl->provider, &http_status, error_msg, sizeof(error_msg));
                if (http_status > 0) {
                    printf("Error: API request failed (HTTP %d): %s\n", http_status, error_msg);
                } else {
                    printf("Error: %s\n", error_msg[0] ? error_msg : "Failed to get response");
                }
                break;
            }

            int tool_count = repl->pending_tool_count;

            // Save assistant text to history
            if (repl->text_accum && repl->text_len > 0) {
                history_add_assistant(&repl->history, repl->text_accum);
            }

            // If no tool calls, check compaction and done
            if (tool_count == 0) {
                if (history_needs_compact(&repl->history)) {
                    printf("\n[Context limit approaching, summarizing history...]\n");
                    repl_run_compaction(repl);
                }
                break;
            }

            // Record tool calls BEFORE executing
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
                fflush(stdout);

                // Reset todo write counter when TodoWrite is called
                if (strcmp(repl->pending_tool_names[i], "TodoWrite") == 0) {
                    todo_list_reset_write_counter();
                }

                char *error = NULL;
                char *result = NULL;

                result = tool_execute(repl->tools, repl->pending_tool_names[i], args, &error);

                if (args) cJSON_Delete(args);

                if (result) {
                    history_add_tool_result(&repl->history, repl->pending_tool_ids[i],
                                    repl->pending_tool_names[i], result);
                    size_t rlen = strlen(result);
                    if (rlen > 50000) rlen = 50000;
                    for (size_t j = 0; j < rlen; j++) {
                        unsigned char c = (unsigned char)result[j];
                        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') result[j] = ' ';
                    }
                    printf("%.*s\n", (int)rlen, result);
                    fflush(stdout);
                    free(result);
                    result = NULL;
                } else if (error) {
                    printf("Error: %s\n", error);
                    // Must add tool result even on error - API requires every tool_use to have a result
                    char errbuf[512];
                    snprintf(errbuf, sizeof(errbuf), "Error: %s", error);
                    history_add_tool_result(&repl->history, repl->pending_tool_ids[i],
                                    repl->pending_tool_names[i], errbuf);
                    free(error);
                    error = NULL;
                } else {
                    // No result and no error - still need to provide a tool result
                    history_add_tool_result(&repl->history, repl->pending_tool_ids[i],
                                    repl->pending_tool_names[i], "(no output)");
                }

            }  // end for i

            if (interrupted) break;

            // Check if the user pressed Ctrl+C during tool execution
            if (repl_is_cancelled(repl)) {
                interrupted = true;
                break;
            }

            // Check compaction
            if (history_needs_compact(&repl->history)) {
                printf("\n[Context limit approaching, summarizing history...]\n");
                repl_run_compaction(repl);
            }

            // Clear pending tools and continue to next iteration
            repl_clear_tools(repl);
            continue;
        }  // end for iteration

        if (iteration >= 100) {
            printf("\n[Warning: Max iterations (100) reached]\n");
        }
    }

    // Remove Ctrl+C handler
    g_repl = NULL;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#else
    signal(SIGINT, SIG_DFL);
#endif

    system_prompt_free(system_prompt);

    return 0;
}

// ============================================================================
// Utility functions
// ============================================================================

