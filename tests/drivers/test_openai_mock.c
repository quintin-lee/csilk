/**
 * @file test_openai_mock.c
 * @brief Unit tests for OpenAI driver using a local mock HTTP server.
 *
 * Starts a Python-based mock server that intercepts /chat/completions
 * and /embeddings endpoints, then exercises the csilk_ai driver against it.
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
#include <sys/wait.h>
#include <unistd.h>

#include <sys/types.h>

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"
#include "csilk/drivers/ai.h"

/* Max time (ms) to wait for the mock server to bind an ephemeral port and
 * report it back over the readiness pipe. */
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
    /* SIGTERM is handled by Python as immediate process termination. Reap
     * the child so no zombie lingers and the port is released. If it is
     * wedged for any reason, escalate to SIGKILL after a short grace. */
    int reaped = 0;
    for (int i = 0; i < 100; i++) { /* up to ~5s */
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
        /* Detach stdin/stdout/stderr from CTest pipes so a failing parent
         * never blocks CTest waiting for child EOF. */
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
        fprintf(stderr, "FATAL: mock server exited early: status=%d\n", status);
    } else {
        fprintf(stderr,
                "FATAL: mock server port not ready within %d ms; aborting test\n",
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
    req.timeout_ms = 10000;

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
    req.timeout_ms = 10000;

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
    req.timeout_ms = 10000;

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
    req.timeout_ms = 10000;

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

typedef struct {
    int  called;
    int  status;
    char content[256];
    int  total_tokens;
} async_chat_state_t;

static void
on_async_chat_done(int status, csilk_ai_chat_response_t* res, void* user_data)
{
    async_chat_state_t* st = (async_chat_state_t*)user_data;
    st->called = 1;
    st->status = status;
    if (res && res->content) {
        snprintf(st->content, sizeof(st->content), "%s", res->content);
    }
    if (res) {
        st->total_tokens = res->total_tokens;
    }
}

typedef struct {
    int    called;
    int    status;
    size_t count;
    size_t dimension;
    float  first_val;
    float  last_val;
} async_emb_state_t;

static void
on_async_emb_done(int status, csilk_ai_embeddings_response_t* res, void* user_data)
{
    async_emb_state_t* st = (async_emb_state_t*)user_data;
    st->called = 1;
    st->status = status;
    if (res) {
        st->count = res->count;
        st->dimension = res->dimension;
        if (res->values && res->dimension >= 4) {
            st->first_val = res->values[0];
            st->last_val = res->values[3];
        }
    }
}

static void
test_chat_async(void)
{
    printf("Testing chat completion (async)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    async_chat_state_t st = {0};
    {
        csilk_ai_message_t      msg = {.role = "user", .content = "Hello async"};
        csilk_ai_chat_request_t req = {0};
        req.model = "gpt-3.5-turbo";
        req.messages = &msg;
        req.message_count = 1;
        req.timeout_ms = 10000;
        csilk_ai_chat_async(ai, &req, on_async_chat_done, &st);
    }

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(st.called == 1);
    assert(st.status == 0);
    assert(strcmp(st.content, "Mock response from test server.") == 0);
    assert(st.total_tokens == 15);

    csilk_ai_free(ai);
    printf("  passed\n");
}

static void
test_embeddings_async(void)
{
    printf("Testing embeddings (async)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "mock-key", get_base_url());
    assert(ai != NULL);

    const char*       input = "Hello async embedding";
    async_emb_state_t st = {0};
    csilk_ai_embeddings_async(ai, "text-embedding-ada-002", &input, 1, on_async_emb_done, &st);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
    assert(st.called == 1);
    assert(st.status == 0);
    assert(st.count == 1);
    assert(st.dimension == 4);
    assert(st.first_val == 0.1f);
    assert(st.last_val == 0.4f);

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
    atexit(stop_mock_server);
    signal(SIGABRT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGINT, on_fatal_signal);

    start_mock_server();

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
    test_chat_async();
    test_embeddings_async();
    test_free_null_ai();

    stop_mock_server();

    printf("All test_openai_mock tests passed successfully!\n");
    return 0;
}
