#ifndef RGREP_H
#define RGREP_H

#include <stdbool.h>

// Output modes
typedef enum {
    OUTPUT_CONTENT,
    OUTPUT_FILES,
    OUTPUT_COUNT
} OutputMode;

// Search configuration
typedef struct {
    char *pattern;           // Search pattern (regex)
    char *path;             // Path to search
    char *glob;             // Glob pattern filter
    char *file_type;        // File type filter (e.g., "go", "py")
    int context;             // Lines of context before/after
    int max_count;           // Max matches per file (0 = unlimited)
    int max_results;         // Max total results (0 = unlimited)
    bool case_sensitive;     // Case sensitive matching
    bool include_binary;     // Include binary files
    int max_line_length;    // Max line length to include
    OutputMode output_mode;  // Output format
} RGrepConfig;

// Search result
typedef struct {
    int files_scanned;
    int files_matched;
    int total_matches;
    struct {
        char *data;
        size_t len;
        size_t capacity;
    } buf;
} RGrepResult;

// Search function
RGrepResult *rgrep_search(RGrepConfig *cfg);

// Free result
void rgrep_free_result(RGrepResult *result);

// Get output string
char *rgrep_get_output(RGrepResult *result);

#endif // RGREP_H
