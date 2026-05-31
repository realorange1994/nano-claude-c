#ifndef GLOB_H
#define GLOB_H

#include <stdbool.h>
#include "buffer.h"

// Type filter
typedef enum {
    GLOB_TYPE_FILE,
    GLOB_TYPE_DIR,
    GLOB_TYPE_ALL
} GlobType;

// Search configuration
typedef struct {
    char *pattern;           // Glob pattern (e.g., "*.go", "**/*.txt")
    char *path;             // Directory to search (default: ".")
    GlobType type_filter;   // Type filter: file, dir, all
    int max_results;        // Maximum results (0 = unlimited)
    bool exclude_hidden;    // Exclude hidden files
} GlobConfig;

// Search result
typedef struct {
    Buffer buf;             // Result buffer
    int count;             // Number of matches
} GlobResult;

// Search function
GlobResult *glob_search(GlobConfig *cfg);

// Free result
void glob_free_result(GlobResult *result);

// Get output string
char *glob_get_output(GlobResult *result);

#endif // GLOB_H
