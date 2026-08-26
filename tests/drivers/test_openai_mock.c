/**
 * @file test_openai_mock.c
 * @brief Unit tests for OpenAI driver using a local mock HTTP server.
 *
 * Starts a Python-based mock server that intercepts /chat/completions
 * and /embeddings endpoints, then exercises the csilk_ai driver against it.
 * @copyright MIT License
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

#define MOCK_PORT 18081

static pid_t g_mock_pid = 0;

static void
start_mock_server(void)
{
    g_mock_pid = fork();
    if (g_mock_pid == 0) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", MOCK_PORT);
        const char* script_path = "tests/drivers/openai_mock_server.py";
        if (access(script_path, R_OK) != 0) {
            script_path = "../tests/drivers/openai_mock_server.py";
        }
        execlp("python3", "python3", script_path, port_str, (char*)NULL);
        _exit(127);
    }
    usleep(300000); /* 300ms to let server start */
}

static void
stop_mock_server(void)
{
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGTERM);
        waitpid(g_mock_pid, NULL, WNOHANG);
        g_mock_pid = 0;
    }
}

static const char*
get_base_url(void)
{
    static char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", MOCK_PORT);
    return url;
}

/* ---- Streaming chunk accumulator ---- */
typedef struct {
    char   buf[1024];
    size_t len;
} stream_accum_t;

static void
stream_cb(const char* chunk, void* user_data)
{
    if (!chunk || !user_data) {
        return;
    }
    stream_accum_t* acc = (stream_accum_t*)user_data;
    size_t          clen = strlen(chunk);
    if (acc->len + clen + 1 <= sizeof(acc->buf)) {
        memcpy(acc->buf + acc->len, chunk, clen);
        acc->len += clen;
        acc->buf[acc->len] = '\0';
    }
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

static void
test_init_null_key(void)
{
    printf("Testing init with NULL api_key...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", NULL, get_base_url());
    assert(ai == NULL);
    printf("  passed\n");
}

static void
test_init_invalid_driver(void)
{
    printf("Testing invalid driver name...\n");
    csilk_ai_t* ai = csilk_ai_new("nonexistent", "key", get_base_url());
    assert(ai == NULL);
    printf("  passed\n");
}

static void
test_chat_non_streaming(void)
{
    printf("Testing chat completion (non-streaming)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t      msg = {.role = "user", .content = "Hello"};
    csilk_ai_chat_request_t req = {0};
    req.model = "gpt-3.5-turbo";
    req.messages = &msg;
    req.message_count = 1;
    req.temperature = 0.7;
    req.max_tokens = 50;

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    assert(strcmp(res.content, "Mock response from test server.") == 0);
    assert(res.prompt_tokens == 10);
    assert(res.completion_tokens == 5);
    assert(res.total_tokens == 15);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_streaming(void)
{
    printf("Testing chat completion (streaming)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t      msg = {.role = "user", .content = "Hello"};
    csilk_ai_chat_request_t req = {0};
    req.messages = &msg;
    req.message_count = 1;
    req.stream = 1;

    stream_accum_t acc = {.buf = {0}, .len = 0};
    req.on_chunk = stream_cb;
    req.user_data = &acc;

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL);
    assert(strcmp(res.content, "Hello world!") == 0);
    assert(strcmp(acc.buf, "Hello world!") == 0);
    assert(res.prompt_tokens == 10);
    assert(res.completion_tokens == 5);
    assert(res.total_tokens == 15);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_streaming_with_tools(void)
{
    printf("Testing chat completion streaming with tool calls...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msg = {
        .role = "user",
        .content = "What's the weather in Beijing?",
    };

    csilk_ai_tool_function_t fn = {
        .name = "get_weather",
        .description = "Get weather for a city",
    };
    csilk_ai_tool_t tool = {.type = "function", .function = fn};

    csilk_ai_chat_request_t req = {0};
    req.messages = &msg;
    req.message_count = 1;
    req.tools = &tool;
    req.tool_count = 1;
    req.stream = 1;

    stream_accum_t acc = {.buf = {0}, .len = 0};
    req.on_chunk = stream_cb;
    req.user_data = &acc;

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.tool_call_count == 1);
    assert(strcmp(res.tool_calls[0].name, "get_weather") == 0);
    assert(strcmp(res.tool_calls[0].id, "call_mock_001") == 0);
    assert(strstr(res.tool_calls[0].arguments, "Beijing") != NULL);
    assert(res.prompt_tokens == 12);
    assert(res.completion_tokens == 8);
    assert(res.total_tokens == 20);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_with_tools(void)
{
    printf("Testing chat with tool calls...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msg = {
        .role = "user",
        .content = "What's the weather in Beijing?",
    };

    csilk_ai_tool_function_t fn = {
        .name = "get_weather",
        .description = "Get weather for a city",
    };
    csilk_ai_tool_t tool = {.type = "function", .function = fn};

    csilk_ai_chat_request_t req = {0};
    req.messages = &msg;
    req.message_count = 1;
    req.tools = &tool;
    req.tool_count = 1;

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.tool_call_count == 1);
    assert(strcmp(res.tool_calls[0].name, "get_weather") == 0);
    assert(strcmp(res.tool_calls[0].id, "call_mock_001") == 0);
    assert(strstr(res.tool_calls[0].arguments, "Beijing") != NULL);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_null_request(void)
{
    printf("Testing chat with NULL request...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, NULL, &res);
    assert(rc == -1);

    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_null_response(void)
{
    printf("Testing chat with NULL response...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t      msg = {.role = "user", .content = "Hi"};
    csilk_ai_chat_request_t req = {0};
    req.messages = &msg;
    req.message_count = 1;

    int rc = csilk_ai_chat(ai, &req, NULL);
    assert(rc == -1);

    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_embeddings_single(void)
{
    printf("Testing embeddings (single)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    const char*                    input = "Hello world";
    csilk_ai_embeddings_response_t res = {0};
    int rc = csilk_ai_embeddings(ai, "text-embedding-ada-002", &input, 1, &res);
    assert(rc == 0);
    assert(res.count == 1);
    assert(res.dimension == 4);
    assert(res.values != NULL);
    assert(res.prompt_tokens == 2);
    assert(res.values[0] == 0.1f);
    assert(res.values[3] == 0.4f);

    csilk_ai_embeddings_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_embeddings_batch(void)
{
    printf("Testing embeddings (batch)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    const char*                    inputs[] = {"Hello", "World", "Test"};
    csilk_ai_embeddings_response_t res = {0};
    int rc = csilk_ai_embeddings(ai, "text-embedding-ada-002", inputs, 3, &res);
    assert(rc == 0);
    assert(res.count == 3);
    assert(res.dimension == 4);
    assert(res.values != NULL);

    csilk_ai_embeddings_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_embeddings_null_input(void)
{
    printf("Testing embeddings with NULL input...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_embeddings_response_t res = {0};
    int                            rc = csilk_ai_embeddings(ai, NULL, NULL, 0, &res);
    assert(rc == -1);

    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_embeddings_null_response(void)
{
    printf("Testing embeddings with NULL response...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    const char* input = "hello";
    int         rc = csilk_ai_embeddings(ai, "ada", &input, 1, NULL);
    assert(rc == -1);

    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_free_null_ai(void)
{
    printf("Testing csilk_ai_free with NULL...\n");
    csilk_ai_free(NULL);
    printf("  passed\n");
}

/* ================================================================== */

int
main(void)
{
    start_mock_server();
    if (g_mock_pid == 0) {
        fprintf(stderr, "Failed to start mock server\n");
        return 1;
    }

    test_init_null_key();
    test_init_invalid_driver();
    test_chat_non_streaming();
    test_chat_streaming();
    test_chat_streaming_with_tools();
    test_chat_with_tools();
    test_chat_null_request();
    test_chat_null_response();
    test_embeddings_single();
    test_embeddings_batch();
    test_embeddings_null_input();
    test_embeddings_null_response();
    test_free_null_ai();

    stop_mock_server();

    printf("All test_openai_mock tests passed successfully!\n");
    return 0;
}
