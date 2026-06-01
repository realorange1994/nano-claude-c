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

### Linux (GCC, dynamic)

```bash
make
```

### Linux (musl static build)

Produce a fully static binary with no glibc dependency, portable across all x86_64 Linux distributions.

#### Prerequisites

- musl cross-compilation toolchain: <https://musl.cc/x86_64-linux-musl-cross.tgz>
- Source tarballs for dependencies (already included in `deps/`):
  - zlib 1.3.1
  - OpenSSL 3.3.2
  - curl (git master, 8.21.0-dev)

#### Step 1: Install the musl cross toolchain

```bash
cd /home/work
curl -LO https://musl.cc/x86_64-linux-musl-cross.tgz
tar xzf x86_64-linux-musl-cross.tgz
# The compiler is now at: /home/work/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
```

#### Step 2: Build zlib with musl

```bash
export MUSL_CC=/home/work/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
cd deps/zlib-1.3.1
./configure --prefix=$PWD/../zlib-install --static CC=$MUSL_CC
make -j$(nproc) && make install
```

#### Step 3: Build OpenSSL with musl

```bash
cd deps/openssl-3.3.2
MUSL_SYSROOT=/home/work/x86_64-linux-musl-cross/x86_64-linux-musl
$MUSL_CC ./Configure linux-x86_64 \
  --prefix=$PWD/../openssl-install \
  --with-cross-compile-prefix=/home/work/x86_64-linux-musl-cross/bin/x86_64-linux-musl- \
  no-shared \
  -DOPENSSL_NO_GNU_STRERROR_R \
  -isystem $PWD/../kernel-include
make -j$(nproc) && make install_sw
```

The `-DOPENSSL_NO_GNU_STRERROR_R` flag prevents OpenSSL from using the GNU-specific `strerror_r` semantics, which avoids pulling in glibc-specific symbols. The `-isystem` for `kernel-include` ensures Linux kernel headers are found before the host system's headers.

#### Step 4: Build libcurl with musl

```bash
cd deps/curl
mkdir -p build-musl && cd build-musl
cmake .. \
  -DCMAKE_C_COMPILER=$MUSL_CC \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_CURL_EXE=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DUSE_NGHTTP2=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DOPENSSL_ROOT_DIR=$PWD/../../openssl-install \
  -DZLIB_ROOT=$PWD/../../zlib-install \
  -DCMAKE_C_FLAGS="-isystem $PWD/../../kernel-include" \
  -DCMAKE_INSTALL_PREFIX=$PWD/../../curl-install
make -j$(nproc) && make install
```

#### Step 5: Build nanoclaude-c

The Makefile is already configured for musl static linking. Just run:

```bash
cd /home/work/nanoclaude-c
make clean && make
```

The resulting `nanoclaude-c` binary is a fully static ELF executable:

```
$ file nanoclaude-c
nanoclaude-c: ELF 64-bit LSB pie executable, x86-64, static-pie linked

$ ldd nanoclaude-c
statically linked
```

No `__isoc23_*` or other glibc-specific symbols are present, ensuring maximum portability across Linux distributions.

#### Troubleshooting

- **`__isoc23_*` symbols in binary**: This means one of the dependencies was compiled with the host glibc instead of the musl cross compiler. Rebuild the offending library using the musl cross toolchain as described above.
- **Missing kernel headers**: The `deps/kernel-include/` directory contains Linux kernel headers (`asm/`, `asm-generic/`, `linux/`) needed by musl. If missing, copy them from the musl toolchain's sysroot or from `/usr/include` on the host.
- **`OPENSSL_NO_GNU_STRERROR_R`**: Without this define, OpenSSL's `o_str.c` will use GNU `strerror_r` semantics (which return `char*`), but musl only provides the POSIX version (which returns `int`). This causes link errors or incorrect behavior.

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
