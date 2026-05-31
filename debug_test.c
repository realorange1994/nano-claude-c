#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/tool.h"
#include "src/buffer.h"
#include "src/history.h"
#include "src/provider.h"

int main() {
    printf("Starting test...\n");
    fflush(stdout);
    
    // Test 1: History
    printf("Test 1: Creating history...\n");
    fflush(stdout);
    
    History h;
    history_init(&h, 16384, 20000);
    
    printf("Test 1a: Adding user message...\n");
    fflush(stdout);
    history_add(&h, MSG_USER, "Hello!");
    
    printf("Test 1b: Adding assistant message...\n");
    fflush(stdout);
    history_add(&h, MSG_ASSISTANT, "Hi there!");
    
    printf("Test 1c: Adding tool message...\n");
    fflush(stdout);
    history_add_tool(&h, "tool_123", "Read", "File contents");
    
    printf("Test 1d: Converting to messages...\n");
    fflush(stdout);
    cJSON *msgs = history_to_messages(&h);
    
    if (msgs) {
        printf("Test 1e: Printing JSON...\n");
        fflush(stdout);
        char *json_str = cJSON_Print(msgs);
        if (json_str) {
            printf("JSON:\n%s\n", json_str);
            free(json_str);
        }
        cJSON_Delete(msgs);
    } else {
        printf("ERROR: messages is NULL!\n");
    }
    
    printf("Test 1f: Freeing history...\n");
    fflush(stdout);
    history_free(&h);
    
    printf("\nTest 2: Provider\n");
    fflush(stdout);
    
    Provider *p = provider_new(PROVIDER_ANTHROPIC, "test", "claude-3-sonnet", NULL);
    if (p) {
        printf("Provider created: model=%s\n", provider_model(p));
        provider_free(p);
    } else {
        printf("ERROR: Failed to create provider!\n");
    }
    
    printf("\nAll tests completed!\n");
    return 0;
}
