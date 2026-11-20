/*
 * Copyright (c) 2026.
 *
 * Embedded llama.cpp AI driver tests.
 * Validates initialization, chat completion, streaming, thinking/reasoning mode,
 * tool calling, ReAct agent loop integration, and vector embeddings.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/core/json/json.h"
#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"
#include "csilk/drivers/ai/tool_registry.h"

/* --- Test Context and Helpers --- */

typedef struct {
    char   accum[2048];
    size_t len;
} stream_buffer_t;

static void
stream_chunk_collector(const char* chunk, void* user_data)
{
    if (!chunk || !user_data) {
        return;
    }
    stream_buffer_t* buf = (stream_buffer_t*)user_data;
    size_t           chunk_len = strlen(chunk);
    if (buf->len + chunk_len < sizeof(buf->accum) - 1) {
        memcpy(buf->accum + buf->len, chunk, chunk_len);
        buf->len += chunk_len;
        buf->accum[buf->len] = '\0';
    }
}

static char*
mock_weather_tool(const char* args_json, void* user_data)
{
    (void)user_data;
    csilk_json_t* obj = csilk_json_parse(args_json);
    const char*   city = "Unknown";
    if (obj) {
        const char* val = csilk_json_get_string(obj, "city");
        if (val) {
            city = val;
        }
    }
    char res[128];
    snprintf(
        res, sizeof(res), "{\"city\":\"%s\",\"temperature_c\":24,\"condition\":\"Sunny\"}", city);
    if (obj) {
        csilk_json_free(obj);
    }
    return strdup(res);
}

/* --- Test Cases --- */

static void
test_llama_lifecycle_and_availability(void)
{
    /* Test factory csilk_ai_new_llama */
    csilk_ai_t* ai1 = csilk_ai_new_llama("models/test.gguf", 2, 1024);
    assert(ai1 != NULL);
    csilk_ai_free(ai1);

    /* Test generic csilk_ai_new with "llama" */
    csilk_ai_t* ai2 = csilk_ai_new("llama", NULL, "models/test2.gguf");
    assert(ai2 != NULL);
    csilk_ai_free(ai2);

    /* Test generic csilk_ai_new with alias "llama.cpp" */
    csilk_ai_t* ai3 = csilk_ai_new("llama.cpp", NULL, NULL);
    assert(ai3 != NULL);
    csilk_ai_free(ai3);

    /* Test availability query */
    int avail = csilk_ai_llama_is_available();
    assert(avail == 0 || avail == 1);
}

static void
test_llama_chat_basic(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    csilk_ai_message_t msgs[2] = {
        {.role = "system", .content = "You are a helpful embedded assistant."},
        {.role = "user",   .content = "Hello, llama engine!"                 },
    };

    csilk_ai_chat_request_t req = {
        .model = "default",
        .messages = msgs,
        .message_count = 2,
        .temperature = 0.7,
        .max_tokens = 64,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    assert(strlen(res.content) > 0);
    assert(res.prompt_tokens > 0);
    assert(res.completion_tokens > 0);
    assert(res.total_tokens == res.prompt_tokens + res.completion_tokens);
    assert(res.tool_call_count == 0);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
}

static void
test_llama_chat_reasoning(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    csilk_ai_message_t msgs[1] = {
        {.role = "user", .content = "Solve this complex puzzle step by step."},
    };

    stream_buffer_t reason_buf = {0};

    csilk_ai_chat_request_t req = {
        .model = "default",
        .messages = msgs,
        .message_count = 1,
        .has_enable_thinking = true,
        .enable_thinking = true,
        .stream = true,
        .on_reasoning_chunk = stream_chunk_collector,
        .user_data = &reason_buf,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.reasoning_content != NULL);
    assert(strlen(res.reasoning_content) > 0);
    assert(reason_buf.len > 0);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
}

static void
test_llama_chat_streaming(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    csilk_ai_message_t msgs[1] = {
        {.role = "user", .content = "Streaming test prompt with multiple words"},
    };

    stream_buffer_t stream_buf = {0};

    csilk_ai_chat_request_t req = {
        .model = "default",
        .messages = msgs,
        .message_count = 1,
        .stream = true,
        .on_chunk = stream_chunk_collector,
        .user_data = &stream_buf,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    assert(stream_buf.len > 0);
    assert(strcmp(stream_buf.accum, res.content) == 0);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
}

static void
test_llama_tool_calling(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    csilk_ai_tool_t tool = {
        .type = "function",
        .function =
            {
                       .name = "get_weather",
                       .description = "Get current weather in a given city",
                       .parameters_json = NULL,
                       },
    };

    csilk_ai_message_t msgs[1] = {
        {.role = "user", .content = "Please get_weather for Tokyo"},
    };

    csilk_ai_chat_request_t req = {
        .model = "default",
        .messages = msgs,
        .message_count = 1,
        .tools = &tool,
        .tool_count = 1,
        .tool_choice = "auto",
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.tool_call_count == 1);
    assert(res.tool_calls != NULL);
    assert(strcmp(res.tool_calls[0].name, "get_weather") == 0);
    assert(strstr(res.tool_calls[0].arguments, "Tokyo") != NULL);

    /* Follow up with tool result */
    csilk_ai_message_t follow_up_msgs[3] = {
        msgs[0],
        {
             .role = "assistant",
             .content = NULL,
             .tool_calls = res.tool_calls,
             .tool_call_count = res.tool_call_count,
             },
        {
             .role = "tool",
             .content = "{\"city\":\"Tokyo\",\"temperature_c\":22,\"condition\":\"Sunny\"}",
             .tool_call_id = res.tool_calls[0].id,
             },
    };

    csilk_ai_chat_request_t req2 = {
        .model = "default",
        .messages = follow_up_msgs,
        .message_count = 3,
        .tools = &tool,
        .tool_count = 1,
    };

    csilk_ai_chat_response_t res2 = {0};
    int                      rc2 = csilk_ai_chat(ai, &req2, &res2);
    assert(rc2 == 0);
    assert(res2.tool_call_count == 0);
    assert(res2.content != NULL);
    assert(strstr(res2.content, "Tokyo") != NULL);

    csilk_ai_chat_response_free(&res);
    csilk_ai_chat_response_free(&res2);
    csilk_ai_free(ai);
}

static void
test_llama_react_agent_loop_integration(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    csilk_ai_tool_registry_t* reg = csilk_ai_tool_registry_new();
    assert(reg != NULL);

    const char* schema = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},"
                         "\"required\":[\"city\"]}";

    assert(csilk_ai_tool_registry_register(
               reg, "get_weather", "Fetch weather information", schema, mock_weather_tool, NULL) ==
           0);

    char* final_text = NULL;
    int   rc = csilk_ai_react_execute_loop(ai,
                                         "llama",
                                         "System: You are an autonomous agent with tools.",
                                         "Please execute tool get_weather for Tokyo and summarize.",
                                         reg,
                                         5,
                                         NULL,
                                         NULL,
                                         &final_text);

    assert(rc == 0);
    assert(final_text != NULL);
    assert(strstr(final_text, "Tokyo") != NULL);

    free(final_text);
    csilk_ai_tool_registry_free(reg);
    csilk_ai_free(ai);
}

static void
test_llama_embeddings(void)
{
    csilk_ai_t* ai = csilk_ai_new_llama(NULL, 0, 0);
    assert(ai != NULL);

    const char* inputs[] = {
        "csilk embedded inference engine",
        "vector database indexing and similarity search",
        "high performance C23 web micro-framework",
    };

    csilk_ai_embeddings_response_t res = {0};
    int                            rc = csilk_ai_embeddings(ai, "default", inputs, 3, &res);
    assert(rc == 0);
    assert(res.count == 3);
    assert(res.dimension == 128);
    assert(res.values != NULL);

    /* Verify L2 normalization */
    for (size_t i = 0; i < res.count; i++) {
        float sum_sq = 0.0f;
        for (size_t d = 0; d < res.dimension; d++) {
            float v = res.values[i * res.dimension + d];
            sum_sq += v * v;
        }
        assert(fabsf(sum_sq - 1.0f) < 1e-3f);
    }

    csilk_ai_embeddings_response_free(&res);
    csilk_ai_free(ai);
}

int
main(void)
{
    printf("Running embedded llama.cpp driver test suite...\n");

    test_llama_lifecycle_and_availability();
    printf("  [PASS] test_llama_lifecycle_and_availability\n");

    test_llama_chat_basic();
    printf("  [PASS] test_llama_chat_basic\n");

    test_llama_chat_reasoning();
    printf("  [PASS] test_llama_chat_reasoning\n");

    test_llama_chat_streaming();
    printf("  [PASS] test_llama_chat_streaming\n");

    test_llama_tool_calling();
    printf("  [PASS] test_llama_tool_calling\n");

    test_llama_react_agent_loop_integration();
    printf("  [PASS] test_llama_react_agent_loop_integration\n");

    test_llama_embeddings();
    printf("  [PASS] test_llama_embeddings\n");

    printf("All embedded llama.cpp driver tests passed successfully!\n");
    return 0;
}
