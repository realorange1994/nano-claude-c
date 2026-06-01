# nanoclaude-c

A minimal Claude Code CLI client written in C, inspired by [Claude Code](https://claude.ai/code).

## Features

- **REPL interface** — interactive conversation with Claude API
- **Built-in tools** — Read, Write, Edit, Grep, Glob, Shell, Calc
- **MCP support** — Model Context Protocol for extending tool capabilities
- **Multi-provider** — Anthropic, OpenAI, DeepSeek, and OpenAI-compatible APIs
- **Streaming output** — real-time streaming responses
- **Conversation history** — persistent chat history with session management
- **Tool use** — full agentic tool-calling loop with automatic execution
- **Cross-platform** — Windows (MSVC) and Linux (GCC/Clang)

## Building

### Windows (MSVC)

```batch
quick_build.bat
```

### Linux (GCC)

```bash
make
```

## Configuration

Edit `~/.nanoclaude/config.json`:

```json
{
  "provider": "anthropic",
  "api_key": "your-api-key",
  "model": "claude-sonnet-4-20250514"
}
```

Supported providers: `anthropic`, `openai`, `deepseek`, or any OpenAI-compatible endpoint via `base_url`.

## Architecture

```
src/
  main.c        — entry point, REPL loop
  repl.c        — conversation flow, tool execution loop
  config.c      — configuration loading
  provider.c    — API provider abstraction
  http.c        — HTTP client (WinHTTP / libcurl)
  tool.c        — tool schema registration & dispatch
  tool_impl.c   — tool implementations (read/write/edit/grep/glob/shell/calc)
  buffer.c      — dynamic string buffer
  rgrep.c       — ripgrep-like search
  glob.c        — file globbing
  jsonrpc.c     — JSON-RPC for MCP
  history.c     — conversation history
  calc.c        — expression calculator
```

## License

MIT
