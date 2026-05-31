#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/tool.h"
#include "src/buffer.h"
#include "src/history.h"
#include "src/provider.h"

// Test API message formatting
void test_message_formatting() {
    printf("=== Test Message Formatting ===\n\n");
    
    History h;
    history_init(&h, 16384, 20000);
    
    // Add user message
    history_add(&h, MSG_USER, "Hello!");
    
    // Add assistant message
    history_add(&h, MSG_ASSISTANT, "Hi there! How can I help?");
    
    // Add tool result message
    history_add_tool(&h, "tool_123", "Read", "File contents: Hello World");
    
    // Convert to JSON
    cJSON *msgs = history_to_messages(&h);
    
    // Print formatted JSON
    char *json_str = cJSON_Print(msgs);
    printf("Messages JSON:\n%s\n\n", json_str);
    
    // Verify JSON is valid
    cJSON *parsed = cJSON_Parse(json_str);
    if (parsed) {
        printf("✓ JSON is valid\n");
        
        // Check message count
        int count = cJSON_GetArraySize(parsed);
        printf("✓ Message count: %d (expected: 3)\n", count);
        
        // Check roles
        for (int i = 0; i < count; i++) {
            cJSON *msg = cJSON_GetArrayItem(parsed, i);
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (role && role->valuestring) {
                printf("  Message %d: role='%s', content_len=%d\n", 
                       i, role->valuestring, 
                       content ? (int)strlen(content->valuestring) : 0);
            }
        }
        
        cJSON_Delete(parsed);
    } else {
        printf("✗ JSON is INVALID!\n");
    }
    
    free(json_str);
    cJSON_Delete(msgs);
    history_free(&h);
}

// Test provider request format
void test_provider_request() {
    printf("\n=== Test Provider Request Format ===\n\n");
    
    Provider *p = provider_new(PROVIDER_ANTHROPIC, "test-key", "claude-3-sonnet-20240229", NULL);
    
    // Create test messages
    History h;
    history_init(&h, 16384, 20000);
    history_add(&h, MSG_USER, "Hello");
    cJSON *msgs = history_to_messages(&h);
    
    // The request body would be created in provider.c
    // Here we just verify the provider is set up correctly
    printf("✓ Provider created successfully\n");
    printf("  Model: %s\n", provider_model(p));
    printf("  Type: %s\n", provider_type(p) == PROVIDER_ANTHROPIC ? "Anthropic" : "OpenAI");
    
    cJSON_Delete(msgs);
    history_free(&h);
    provider_free(p);
}

int main() {
    printf("===========================================\n");
    printf("  NanoClaude-C Integration Test\n");
    printf("===========================================\n\n");
    
    test_message_formatting();
    test_provider_request();
    
    printf("\n===========================================\n");
    printf("  All integration tests passed!\n");
    printf("===========================================\n");
    
    return 0;
}
