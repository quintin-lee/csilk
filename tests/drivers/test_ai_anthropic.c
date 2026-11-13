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
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

#define MOCK_START_TIMEOUT_MS 15000

static volatile sig_atomic_t g_mock_pid = 0;
static int                   g_mock_port = 0;
static int                   g_parent_pipe_wr = -1;

static void
on_fatal_signal(int sig)
{
    if (g_parent_pipe_wr >= 0) {
        close(g_parent_pipe_wr);
        g_parent_pipe_wr = -1;
    }
    pid_t pid = (pid_t)g_mock_pid;
    if (pid > 0) {
        kill(pid, SIGKILL);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
stop_mock_server(void)
{
    if (g_parent_pipe_wr >= 0) {
        close(g_parent_pipe_wr);
        g_parent_pipe_wr = -1;
    }
    pid_t pid = (pid_t)g_mock_pid;
    if (pid <= 0) {
        return;
    }
    g_mock_pid = 0;
    kill(pid, SIGTERM);
    int status = 0;
    int reaped = 0;
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = 1;
            break;
        }
        usleep(50 * 1000);
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

static const char*
resolve_mock_script(void)
{
    static const char* const candidates[] = {
        "tests/drivers/openai_mock_server.py",
        "../tests/drivers/openai_mock_server.py",
        "../../tests/drivers/openai_mock_server.py",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
    return "../tests/drivers/openai_mock_server.py";
}

static void
start_mock_server(void)
{
    int ready_pipe[2];
    if (pipe(ready_pipe) != 0) {
        perror("pipe");
        abort();
    }
    int parent_pipe[2];
    if (pipe(parent_pipe) != 0) {
        perror("pipe");
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        abort();
    }

    char        ready_fd_str[16];
    char        parent_fd_str[16];
    const char* script_path = resolve_mock_script();
    snprintf(ready_fd_str, sizeof(ready_fd_str), "%d", ready_pipe[1]);
    snprintf(parent_fd_str, sizeof(parent_fd_str), "%d", parent_pipe[0]);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        close(parent_pipe[0]);
        close(parent_pipe[1]);
        abort();
    }
    if (pid == 0) {
        close(ready_pipe[0]);
        close(parent_pipe[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execlp("python3",
               "python3",
               script_path,
               "0",
               "--ready-fd",
               ready_fd_str,
               "--parent-fd",
               parent_fd_str,
               (char*)NULL);
        _exit(127);
    }

    g_mock_pid = (sig_atomic_t)pid;
    g_parent_pipe_wr = parent_pipe[1];
    close(ready_pipe[1]);
    close(parent_pipe[0]);

    char   port_buf[32];
    size_t used = 0;
    int    ready = 0;
    while (used + 1 < sizeof(port_buf)) {
        struct pollfd pfd = {
            .fd = ready_pipe[0],
            .events = POLLIN,
            .revents = 0,
        };
        int prc = poll(&pfd, 1, MOCK_START_TIMEOUT_MS);
        if (prc <= 0) {
            if (prc < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        ssize_t n = read(ready_pipe[0], port_buf + used, sizeof(port_buf) - 1 - used);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        used += (size_t)n;
        port_buf[used] = '\0';
        if (strchr(port_buf, '\n') != NULL) {
            ready = 1;
            break;
        }
    }
    close(ready_pipe[0]);

    if (ready) {
        int port = atoi(port_buf);
        if (port > 0 && port <= 65535) {
            g_mock_port = port;
            return;
        }
    }

    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
        g_mock_pid = 0;
        fprintf(stderr, "FATAL: anthropic mock server exited early: status=%d\n", status);
    } else {
        fprintf(stderr,
                "FATAL: anthropic mock server port not ready within %d ms\n",
                MOCK_START_TIMEOUT_MS);
        stop_mock_server();
    }
    abort();
}

static const char*
get_base_url(void)
{
    static char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", g_mock_port);
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
        .timeout_ms = 10000,
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
        .timeout_ms = 10000,
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
        .timeout_ms = 10000,
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
        .timeout_ms = 10000,
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
    signal(SIGABRT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGINT, on_fatal_signal);
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
