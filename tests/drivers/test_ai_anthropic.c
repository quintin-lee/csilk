/**
 * @file test_ai_anthropic.c
 * @brief Unit tests for the native Anthropic Messages API (Claude) driver.
 *
 * Uses the local Python mock server to test non-streaming, streaming,
 * and tool-calling endpoints for both "anthropic" and "claude" driver aliases.
 * @copyright MIT License
 */

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

#define MOCK_PORT 18082
#define MOCK_START_TIMEOUT_MS 10000
#define MOCK_START_POLL_MS 50

static pid_t g_mock_pid = 0;

static int
probe_mock_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MOCK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    close(fd);
    return rc;
}

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

    for (int waited_ms = 0; waited_ms < MOCK_START_TIMEOUT_MS; waited_ms += MOCK_START_POLL_MS) {
        if (probe_mock_port() == 0) {
            return;
        }
        int status;
        if (waitpid(g_mock_pid, &status, WNOHANG) == g_mock_pid) {
            g_mock_pid = 0;
            fprintf(stderr, "anthropic mock server exited early: status=%d\n", status);
            return;
        }
        usleep(MOCK_START_POLL_MS * 1000);
    }
    fprintf(stderr,
            "FATAL: anthropic mock server port not ready within %d ms\n",
            MOCK_START_TIMEOUT_MS);
    g_mock_pid = 0;
    abort();
}

static void
stop_mock_server(void)
{
    if (g_mock_pid <= 0) {
        return;
    }
    kill(g_mock_pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(g_mock_pid, &status, WNOHANG);
        if (r == g_mock_pid) {
            break;
        }
        usleep(50 * 1000);
    }
    g_mock_pid = 0;
}

static const char*
get_base_url(void)
{
    static char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", MOCK_PORT);
    return url;
}

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

static void
test_init_aliases(void)
{
    printf("Testing Anthropic driver initialization aliases...\n");
    csilk_ai_t* ai1 = csilk_ai_new("anthropic", "mock-key", get_base_url());
    assert(ai1 != NULL);
    csilk_ai_free(ai1);

    csilk_ai_t* ai2 = csilk_ai_new("claude", "mock-key", get_base_url());
    assert(ai2 != NULL);
    csilk_ai_free(ai2);

    /* NULL key rejected */
    assert(csilk_ai_new("anthropic", NULL, get_base_url()) == NULL);
    printf("  passed\n");
}

static void
test_chat_non_streaming(void)
{
    printf("Testing Anthropic chat non-streaming...\n");
    csilk_ai_t* ai = csilk_ai_new("anthropic", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msgs[] = {
        {.role = "system", .content = "You are an assistant."},
        {.role = "user",   .content = "Hello Claude"         }
    };

    csilk_ai_chat_request_t req = {
        .model = "claude-3-5-sonnet-20241022",
        .messages = msgs,
        .message_count = 2,
        .temperature = 0.5,
        .max_tokens = 1024,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL && strstr(res.content, "Hello from Claude mock.") != NULL);
    assert(res.prompt_tokens == 10);
    assert(res.completion_tokens == 6);
    assert(res.total_tokens == 16);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_tools_non_streaming(void)
{
    printf("Testing Anthropic chat with tool use...\n");
    csilk_ai_t* ai = csilk_ai_new("claude", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msg = {.role = "user", .content = "What is the weather in Beijing?"};
    csilk_json_t*      p_schema = csilk_json_object();
    csilk_json_add_string(p_schema, "type", "object");

    csilk_ai_tool_function_t fn = {
        .name = "get_weather",
        .description = "Fetch weather",
        .parameters_json = p_schema,
    };
    csilk_ai_tool_t tool = {.type = "function", .function = fn};

    csilk_ai_chat_request_t req = {
        .model = "claude-3-5-sonnet",
        .messages = &msg,
        .message_count = 1,
        .tools = &tool,
        .tool_count = 1,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.tool_call_count == 1);
    assert(strcmp(res.tool_calls[0].name, "get_weather") == 0);
    assert(strcmp(res.tool_calls[0].id, "toolu_mock_001") == 0);
    assert(strstr(res.tool_calls[0].arguments, "Beijing") != NULL);
    assert(res.prompt_tokens == 12);
    assert(res.completion_tokens == 8);

    csilk_ai_chat_response_free(&res);
    csilk_json_free(p_schema);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_streaming(void)
{
    printf("Testing Anthropic chat streaming...\n");
    csilk_ai_t* ai = csilk_ai_new("anthropic", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msg = {.role = "user", .content = "Hello stream"};
    stream_accum_t     acc = {.buf = {0}, .len = 0};

    csilk_ai_chat_request_t req = {
        .model = "claude-3-5-sonnet",
        .messages = &msg,
        .message_count = 1,
        .stream = true,
        .on_chunk = stream_cb,
        .user_data = &acc,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.content != NULL && strstr(res.content, "Hello Claude!") != NULL);
    assert(strstr(acc.buf, "Hello Claude!") != NULL);
    assert(res.prompt_tokens == 12);
    assert(res.completion_tokens == 5);
    assert(res.total_tokens == 17);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_chat_streaming_tools(void)
{
    printf("Testing Anthropic chat streaming tool use...\n");
    csilk_ai_t* ai = csilk_ai_new("claude", "mock-key", get_base_url());
    assert(ai != NULL);

    csilk_ai_message_t msg = {.role = "user", .content = "Check weather in Beijing"};
    csilk_json_t*      p_schema2 = csilk_json_object();
    csilk_json_add_string(p_schema2, "type", "object");

    csilk_ai_tool_function_t fn = {
        .name = "get_weather",
        .description = "Fetch weather",
        .parameters_json = p_schema2,
    };
    csilk_ai_tool_t tool = {.type = "function", .function = fn};

    csilk_ai_chat_request_t req = {
        .model = "claude-3-5-sonnet",
        .messages = &msg,
        .message_count = 1,
        .tools = &tool,
        .tool_count = 1,
        .stream = true,
    };

    csilk_ai_chat_response_t res = {0};
    int                      rc = csilk_ai_chat(ai, &req, &res);
    assert(rc == 0);
    assert(res.tool_call_count == 1);
    assert(strcmp(res.tool_calls[0].name, "get_weather") == 0);
    assert(strcmp(res.tool_calls[0].id, "toolu_mock_001") == 0);
    assert(strstr(res.tool_calls[0].arguments, "Beijing") != NULL);
    assert(res.prompt_tokens == 12);
    assert(res.completion_tokens == 8);

    csilk_ai_chat_response_free(&res);
    csilk_json_free(p_schema2);
    csilk_ai_free(ai);
    printf("  passed\n");
}

int
main(void)
{
    atexit(stop_mock_server);
    start_mock_server();

    test_init_aliases();
    test_chat_non_streaming();
    test_chat_tools_non_streaming();
    test_chat_streaming();
    test_chat_streaming_tools();

    stop_mock_server();
    printf("All test_ai_anthropic tests passed!\n");
    return 0;
}
