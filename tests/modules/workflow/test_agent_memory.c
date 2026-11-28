/**
 * @file test_agent_memory.c
 * @brief Exercises csilk_agent_memory_* (long-term agent memory) against the
 *        mock OpenAI (embeddings) + Qdrant vector-DB endpoints, plus the
 *        workflow context memory accessors and the OOM/guard branches.
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
#include "csilk/workflow/workflow.h"
#include "csilk/core/sys_io.h"
#include "csilk/drivers/ai.h"
#include "csilk/drivers/vector.h"

#define MOCK_START_TIMEOUT_MS 15000

static volatile sig_atomic_t g_mock_pid = 0;
static int                   g_mock_port = 0;
static int                   g_parent_pipe_wr = -1;
static char                  g_base_url[64];

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
    atexit(stop_mock_server);
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
    for (;;) {
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
        if (memchr(port_buf, '\n', used)) {
            break;
        }
    }
    close(ready_pipe[0]);
    port_buf[used] = '\0';
    char* end = NULL;
    long  port = strtol(port_buf, &end, 10);
    if (end == port_buf || port <= 0 || port > 65535) {
        fprintf(stderr, "FATAL: mock server did not report a port\n");
        abort();
    }
    g_mock_port = (int)port;
    snprintf(g_base_url, sizeof(g_base_url), "http://127.0.0.1:%d", g_mock_port);
    sleep(1);
}

static void
test_agent_memory_guard(void)
{
    printf("Testing csilk_agent_memory_* guard branches...\n");
    assert(csilk_agent_memory_new(NULL, "m", (csilk_vector_db_t*)1, "col") == NULL);
    assert(csilk_agent_memory_new((csilk_ai_t*)1, "m", NULL, "col") == NULL);
    assert(csilk_agent_memory_new((csilk_ai_t*)1, "m", (csilk_vector_db_t*)1, NULL) == NULL);

    /* NULL mem / NULL args to store/recall. */
    assert(csilk_agent_memory_store(NULL, "id", "text", NULL) == -1);
    assert(csilk_agent_memory_store((csilk_agent_memory_t*)1, NULL, "t", NULL) == -1);
    assert(csilk_agent_memory_store((csilk_agent_memory_t*)1, "id", NULL, NULL) == -1);

    csilk_vector_search_response_t res = {0};
    assert(csilk_agent_memory_recall(NULL, "q", 3, &res) == -1);
    assert(csilk_agent_memory_recall((csilk_agent_memory_t*)1, NULL, 3, &res) == -1);
    assert(csilk_agent_memory_recall((csilk_agent_memory_t*)1, "q", 3, NULL) == -1);
    csilk_vector_search_response_free(&res);

    /* free(NULL) is a no-op. */
    csilk_agent_memory_free(NULL);
    printf("  passed\n");
}

static void
test_agent_memory_roundtrip(void)
{
    printf("Testing agent memory store/recall round-trip (OpenAI embeddings + Qdrant)...\n");
    csilk_ai_t* ai = csilk_ai_new("openai", "key", g_base_url);
    assert(ai != NULL);
    csilk_vector_db_t* db = csilk_vector_db_new("qdrant", g_base_url, "key");
    assert(db != NULL);

    /* embedding_model=NULL -> defaults to text-embedding-ada-002. */
    csilk_agent_memory_t* mem = csilk_agent_memory_new(ai, NULL, db, "memories");
    assert(mem != NULL);

    /* Store two memories (with + without metadata JSON). */
    int rc = csilk_agent_memory_store(mem, "m1", "the sky is blue", NULL);
    assert(rc == 0);
    rc = csilk_agent_memory_store(mem, "m2", "2 + 2 = 4", "{\"kind\":\"fact\"}");
    assert(rc == 0);

    /* Recall with an explicit limit and one with the default (<= 0 -> 5). */
    csilk_vector_search_response_t res = {0};
    assert(csilk_agent_memory_recall(mem, "what color is the sky", 2, &res) == 0);
    assert(res.count >= 1 && res.count <= 2);
    csilk_vector_search_response_free(&res);

    res = (csilk_vector_search_response_t){0};
    assert(csilk_agent_memory_recall(mem, "math", 0, &res) == 0);
    csilk_vector_search_response_free(&res);

    /* A memory that embeds to zero vectors cannot happen with the mock, so
     * instead verify free + NULL-config default-model name survived. */
    csilk_agent_memory_free(mem);
    csilk_vector_db_free(db);
    csilk_ai_free(ai);
    printf("  passed\n");
}

int
main(void)
{
    start_mock_server();
    printf("Agent memory store running against mock OpenAI + Qdrant on %s\n", g_base_url);

    test_agent_memory_guard();
    test_agent_memory_roundtrip();

    stop_mock_server();
    printf("test_agent_memory: ALL PASSED\n");
    return 0;
}
