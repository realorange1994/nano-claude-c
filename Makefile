# NanoClaude-C - Linux Makefile
# Cross-platform: Windows uses MSVC (quick_build.bat), Linux uses this Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -O2
LDFLAGS =

# Source files
SRCS = \
	src/main.c \
	src/http.c \
	src/buffer.c \
	src/config.c \
	src/history.c \
	src/provider.c \
	src/calc.c \
	src/glob.c \
	src/rgrep.c \
	src/tool.c \
	src/tool_impl.c \
	src/jsonrpc.c \
	src/repl.c \
	deps/cJSON/cJSON.c

OBJS = $(SRCS:.c=.o)

# Include paths
CFLAGS += -Ideps/cJSON -Isrc

# Linux dependencies
LDFLAGS += -lcurl -lm -lpthread -rdynamic

# Backtrace support
LDFLAGS += -ldl

TARGET = nanoclaude-c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Generic compile rule
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

deps/cJSON/cJSON.o: deps/cJSON/cJSON.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
