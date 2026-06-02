#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include "cJSON.h"

#define MAX_MESSAGES 4096
#define MAX_FILE_PATHS 1024
#define MAX_TOOL_RESULT_LEN 10000
#define CHARS_PER_TOKEN 4

// System-injected marker (HTML comment prefix for cache breakpoint avoidance)
// Matches Go project's SystemInjectedPrefix = "<!-- system-injected -->"
#define SYSTEM_INJECTED_MARKER "<!-- system-injected -->"

// Entry type - distinguishes different kinds of entries (like miniclaude's EntryType)
typedef enum {
    ENTRY_MESSAGE,       // Regular message
    ENTRY_TOOL_RESULT,    // Tool execution result
    ENTRY_COMPACTION,     // Compaction/summary marker
    ENTRY_SYSTEM          // System-injected entry (e.g., time context)
} EntryType;

// Role - same as before
typedef enum {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} Role;

// Message entry - tracks individual conversation entries with full metadata
typedef struct {
    char id[64];                  // Unique ID for this entry
    char parent_id[64];           // ID of parent entry (for linking tool calls to results)
    EntryType type;               // Type of entry
    Role role;                    // Role: user, assistant, tool
    char *content;                // Text content or tool input JSON
    char *tool_name;              // Tool name (if this is a tool call/result)
    char *tool_result;            // Tool result content (for ENTRY_TOOL_RESULT)
} Entry;

// File operation tracking - tracks which files have been read/written/edited
typedef struct {
    char *paths[MAX_FILE_PATHS];
    int count;
} FileSet;

// History - main history manager with context management
typedef struct {
    Entry entries[MAX_MESSAGES];
    int count;
    char *summary;
    int reserve_tokens;
    int keep_recent_tokens;
    int context_window;
    
    // File operation tracking (like miniclaude)
    FileSet read_files;
    FileSet written_files;
    FileSet edited_files;
    
    // Compaction state
    int last_compaction_index;
} History;

// ============================================================================
// History lifecycle
// ============================================================================

void history_init(History *h, int reserve_tokens, int keep_recent_tokens, int context_window);
void history_free(History *h);
void history_reset(History *h);

// ============================================================================
// Message adding (like miniclaude's AddUserMessage, AddAssistantMessage)
// ============================================================================

// Add a user message
void history_add_user(History *h, const char *content);

// Add an assistant message (plain text)
void history_add_assistant(History *h, const char *content);

// Add a tool call entry (assistant side) - tracks file operations
void history_add_tool_call(History *h, const char *tool_id, const char *tool_name, const char *tool_input_json);

// Add tool result entry (user side) - linked to tool call by parent_id
void history_add_tool_result(History *h, const char *parent_id, const char *tool_name, const char *tool_result);

// Add system entry (e.g., time context)
void history_add_system(History *h, const char *content);

// ============================================================================
// Time context injection (like miniclaude's InjectTimeContext)
// ============================================================================

// Inject or update current time context
void history_inject_time_context(History *h);
void history_inject_system(History *h, const char *content);

// ============================================================================
// Compaction/Summarization (like miniclaude's CompactRun)
// ============================================================================

// Check if compaction is needed (like miniclaude's ShouldCompact)
bool history_needs_compact(History *h);

// Find the best cut point for compaction (like miniclaude's FindCutPoint)
// Returns index where we should cut, keeping entries from that point onwards
int history_find_cut_point(History *h);

// Perform compaction - summarize old content and keep recent entries
void history_compact(History *h, char *(*summarize_fn)(const char *));

// ============================================================================
// Message building for API (like miniclaude's GetMessages)
// ============================================================================

// Get messages formatted for API call
cJSON *history_to_messages(History *h);

// ============================================================================
// Token estimation (improved like miniclaude)
// ============================================================================

// Estimate total tokens in current context
int history_estimate_tokens(History *h);

// ============================================================================
// File operation tracking (like miniclaude's GetFileOperations)
// ============================================================================

// Get list of files that were read
const char **history_get_read_files(History *h, int *count);

// Get list of files that were written
const char **history_get_written_files(History *h, int *count);

// Get list of files that were edited
const char **history_get_edited_files(History *h, int *count);

// ============================================================================
// Utility
// ============================================================================

// Format file operations for summarization prompt
char *history_format_file_ops(History *h);

// ============================================================================
// Internal helpers
// ============================================================================

// Generate a unique ID
void history_generate_id(char *buf, size_t bufsize);

// Truncate content for tool results (like miniclaude's truncateContent)
char *history_truncate_content(const char *content, int max_len);

// Get summarization prompt template
const char *history_get_summarization_prompt(void);

#endif // HISTORY_H
