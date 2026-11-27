/**
 * @file test_ollama_mock.c
 * @brief Unit tests for the Ollama AI driver using a local mock HTTP server.
 *
 * Reuses tests/drivers/openai_mock_server.py (which also serves the Ollama
 * /api/chat and /api/embeddings endpoints) to exercise the Ollama driver's
 * non-streaming chat, tool-call parsing, streaming NDJSON, and embeddings
 * paths without requiring a real Ollama runtime.
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
#include "csilk/core/sys_io.h"
#include "csilk/drivers/ai.h"

#define MOCK_START_TIMEOUT_MS 15000

static volatile sig_atomic_t g_mock_pid = 0;
static int                   g_mock_port = 0;
static int                   g_parent_pipe_wr = -1;

static void
on_fatal_signal(int sig)
{
    (void)sig;
    if (g_parent_pipe_wr >= 0) {
        close(g_parent_pipe_wr);
        g_parent_pipe_wr = -1;
    }
}

static void
stop_mock_server(void)
{
    if (g_mock_pid == 0) {
        return;
    }
    kill(g_mock_pid, SIGTERM);
    int status;
    waitpid(g_mock_pid, &status, 0);
    g_mock_pid = 0;
    if (g_parent_pipe_wr >= 0) {
        close(g_parent_pipe_wr);
        g_parent_pipe_wr = -1;
    }
}

static const char*
resolve_mock_script(void)
{
    static const char* const candidates[] = {
        "tests/modules/ai/openai_mock_server.py",
        "../tests/modules/ai/openai_mock_server.py",
        "../../tests/modules/ai/openai_mock_server.py",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }
    return "../tests/modules/ai/openai_mock_server.py";
}

static void
start_mock_server(void)
{
    signal(SIGINT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);

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

    char   port_buf[32] = {0};
    size_t used = 0;
    int    ready = 0;
    while (used + 1 < sizeof(port_buf)) {
        struct pollfd pfd = {.fd = ready_pipe[0], .events = POLLIN, .revents = 0};
        int           prc = poll(&pfd, 1, MOCK_START_TIMEOUT_MS);
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
    stop_mock_server();
    fprintf(stderr, "FATAL: mock server did not report a port\n");
    abort();
}

static const char*
get_base_url(void)
{
    static char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", g_mock_port);
    return url;
}

/* ---- streaming accumulator ---- */
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

int
main(void)
{
    start_mock_server();

    /* 1. Non-streaming chat */
    {
        printf("Testing Ollama chat (non-streaming)...\n");
        csilk_ai_t* ai = csilk_ai_new("ollama", "k", get_base_url());
        assert(ai != NULL);

        csilk_ai_message_t      msg = {.role = "user", .content = "Hi"};
        csilk_ai_chat_request_t req = {0};
        req.model = "llama3";
        req.messages = &msg;
        req.message_count = 1;
        req.timeout_ms = 10000;

        csilk_ai_chat_response_t res = {0};
        int                      rc = csilk_ai_chat(ai, &req, &res);
        assert(rc == 0);
        assert(res.content != NULL);
        assert(strstr(res.content, "Ollama mock reply") != NULL);
        assert(res.total_tokens == 6 + 8);
        csilk_ai_chat_response_free(&res);
        csilk_ai_free(ai);
        printf("  passed\n");
    }

    /* 2. Tool-call chat (non-streaming) */
    {
        printf("Testing Ollama chat tool-call parsing...\n");
        csilk_ai_t* ai = csilk_ai_new("ollama", "k", get_base_url());
        assert(ai != NULL);

        csilk_json_t*   params = csilk_json_parse("{\"type\":\"object\"}");
        csilk_ai_tool_t tools[1] = {0};
        tools[0].function.name = "get_weather";
        tools[0].function.description = "Get weather";
        tools[0].function.parameters_json = params;

        csilk_ai_message_t      msg = {.role = "user", .content = "weather"};
        csilk_ai_chat_request_t req = {0};
        req.model = "llama3";
        req.messages = &msg;
        req.message_count = 1;
        req.tools = tools;
        req.tool_count = 1;
        req.timeout_ms = 10000;

        csilk_ai_chat_response_t res = {0};
        int                      rc = csilk_ai_chat(ai, &req, &res);
        assert(rc == 0);
        assert(res.tool_call_count == 1);
        assert(res.tool_calls[0].name != NULL);
        assert(strstr(res.tool_calls[0].name, "get_weather") != NULL);
        csilk_ai_chat_response_free(&res);
        csilk_json_free(params);
        csilk_ai_free(ai);
        printf("  passed\n");
    }

    /* 3. Streaming NDJSON chat */
    {
        printf("Testing Ollama chat (streaming NDJSON)...\n");
        csilk_ai_t* ai = csilk_ai_new("ollama", "k", get_base_url());
        assert(ai != NULL);

        stream_accum_t          acc = {0};
        csilk_ai_message_t      msg = {.role = "user", .content = "stream"};
        csilk_ai_chat_request_t req = {0};
        req.model = "llama3";
        req.messages = &msg;
        req.message_count = 1;
        req.stream = true;
        req.on_chunk = stream_cb;
        req.user_data = &acc;
        req.timeout_ms = 10000;

        csilk_ai_chat_response_t res = {0};
        int                      rc = csilk_ai_chat(ai, &req, &res);
        assert(rc == 0);
        assert(res.content != NULL);
        assert(strstr(res.content, "Ollama mock") != NULL);
        assert(acc.len > 0);
        csilk_ai_chat_response_free(&res);
        csilk_ai_free(ai);
        printf("  passed\n");
    }

    /* 4. Embeddings (multi-input -> concatenated) */
    {
        printf("Testing Ollama embeddings...\n");
        csilk_ai_t* ai = csilk_ai_new("ollama", "k", get_base_url());
        assert(ai != NULL);

        const char*                    inputs[] = {"hello", "world"};
        csilk_ai_embeddings_response_t res = {0};
        int                            rc = csilk_ai_embeddings(ai, "llama3", inputs, 2, &res);
        assert(rc == 0);
        assert(res.count == 2);
        assert(res.dimension == 4);
        assert(res.values != NULL);
        csilk_ai_embeddings_response_free(&res);
        csilk_ai_free(ai);
        printf("  passed\n");
    }

    /* 5. Null / guard branches */
    {
        printf("Testing Ollama driver guard branches...\n");
        csilk_ai_t* ai = csilk_ai_new("ollama", "k", get_base_url());
        assert(ai != NULL);

        csilk_ai_chat_request_t  req = {0};
        csilk_ai_chat_response_t res = {0};
        /* Empty request (no model, no messages) still runs the Ollama JSON
         * construction + HTTP path; with an unreachable scheme it is a -1. */
        int rc = csilk_ai_chat(ai, &req, &res);
        /* model defaults to llama3 and the mock is reachable -> success. */
        assert(rc == 0);
        csilk_ai_chat_response_free(&res);
        csilk_ai_free(ai);

        /* Bad base URL scheme (SSRF guard) -> chat must fail. */
        csilk_ai_t* bad = csilk_ai_new("ollama", "k", "file:///etc/passwd");
        assert(bad != NULL);
        csilk_ai_message_t      m2 = {.role = "user", .content = "x"};
        csilk_ai_chat_request_t req2 = {0};
        req2.model = "llama3";
        req2.messages = &m2;
        req2.message_count = 1;
        csilk_ai_chat_response_t res2 = {0};
        int                      rc2 = csilk_ai_chat(bad, &req2, &res2);
        assert(rc2 == -1);
        csilk_ai_chat_response_free(&res2);
        csilk_ai_free(bad);
        printf("  passed\n");
    }

    stop_mock_server();
    printf("test_ollama_mock: ALL PASSED\n");
    return 0;
}
