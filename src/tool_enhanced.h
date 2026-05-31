#ifndef TOOL_ENHANCED_H
#define TOOL_ENHANCED_H

// Enhanced tool definitions for nanoclaude-c

// Edit tool: supports batch edits with diff output
// Parameters:
//   - path: file path
//   - edits: array of {oldText, newText}

// Grep tool: supports regex, case sensitivity, context lines
// Parameters:
//   - pattern: regex pattern
//   - path: directory or file path
//   - caseSensitive: bool
//   - context: number of context lines
//   - outputMode: "content" | "files_with_matches" | "count"

// Glob tool: supports recursive ** patterns
// Parameters:
//   - pattern: glob pattern with **
/*
# glob result format
GlobResult.txt
dir/Subdir/file.txt
dir/file2.txt
*/

#endif
