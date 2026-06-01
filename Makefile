# NanoClaude-C - Linux Makefile (musl-gcc static build)

CC = /home/work/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-stringop-truncation -O2
LDFLAGS = -static

SRCS = src/main.c src/http.c src/buffer.c src/config.c src/history.c src/provider.c src/calc.c src/glob.c src/rgrep.c src/tool.c src/tool_impl.c src/jsonrpc.c src/repl.c deps/cJSON/cJSON.c
OBJS = $(SRCS:.c=.o)

CFLAGS += -Ideps/cJSON -Isrc -Ideps/curl-install/include -Ideps/openssl-install/include

CURL_LIB = deps/curl-install/lib/libcurl.a
SSL_LIBS = deps/openssl-install/lib/libssl.a deps/openssl-install/lib/libcrypto.a
ZLIB     = deps/zlib-install/lib/libz.a
LDFLAGS += $(CURL_LIB) $(SSL_LIBS) $(ZLIB) -lm -lpthread -ldl

TARGET = nanoclaude-c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

deps/cJSON/cJSON.o: deps/cJSON/cJSON.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
