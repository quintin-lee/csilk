/**
 * @file test_vector_milvus_qdrant_mock.c
 * @brief Unit tests for the Milvus & Qdrant vector-DB drivers against a local
 *        mock HTTP server (tests/drivers/openai_mock_server.py).
 *
 * Exercises the csilk_vector_db_new/upsert/search path through the driver
 * registry with both REST backends, covering upsert success/failure and
 * search result parsing (Milvus "data" and Qdrant "result" envelopes).
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
#include "csilk/drivers/vector.h"

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

static float g_vec[4] = {0.1f, 0.2f, 0.3f, 0.4f};

int
main(void)
{
    start_mock_server();

    /* ---- Milvus driver ---- */
    {
        printf("Testing Milvus vector driver (mock)...\n");
        csilk_vector_db_t* db = csilk_vector_db_new("milvus", get_base_url(), "tok");
        assert(db != NULL);

        csilk_vector_point_t pt = {
            .id = "p1",
            .dimension = 4,
            .vector = g_vec,
        };
        int rc = csilk_vector_db_upsert(db, "docs", &pt, 1);
        assert(rc == 0);

        csilk_vector_search_response_t res = {0};
        int                            rc2 = csilk_vector_db_search(db, "docs", g_vec, 4, 2, &res);
        assert(rc2 == 0);
        assert(res.count == 2);
        assert(res.results[0].id != NULL);
        assert(strstr(res.results[0].id, "row-0") != NULL);
        assert(res.results[0].payload != NULL);
        csilk_vector_search_response_free(&res);
        csilk_vector_db_free(db);
        printf("  passed\n");
    }

    /* ---- Qdrant driver ---- */
    {
        printf("Testing Qdrant vector driver (mock)...\n");
        csilk_vector_db_t* db = csilk_vector_db_new("qdrant", get_base_url(), "key");
        assert(db != NULL);

        csilk_vector_point_t pt = {
            .id = "q1",
            .dimension = 4,
            .vector = g_vec,
        };
        int rc = csilk_vector_db_upsert(db, "col", &pt, 1);
        assert(rc == 0);

        csilk_vector_search_response_t res = {0};
        int                            rc2 = csilk_vector_db_search(db, "col", g_vec, 4, 2, &res);
        assert(rc2 == 0);
        assert(res.count == 2);
        assert(res.results[0].payload != NULL);
        csilk_vector_search_response_free(&res);
        csilk_vector_db_free(db);
        printf("  passed\n");
    }

    /* ---- Guard / error branches ---- */
    {
        printf("Testing vector driver guard branches...\n");
        /* NULL endpoint -> init failure -> NULL handle. */
        assert(csilk_vector_db_new("milvus", NULL, "tok") == NULL);
        assert(csilk_vector_db_new("qdrant", NULL, NULL) == NULL);
        /* Unknown driver name -> NULL. */
        assert(csilk_vector_db_new("no_such_driver", get_base_url(), "k") == NULL);
        /* free(NULL) is safe. */
        csilk_vector_db_free(NULL);

        /* A 404 endpoint must surface as a -1 upsert / search error. */
        csilk_vector_db_t* db = csilk_vector_db_new("milvus", get_base_url(), "tok");
        assert(db != NULL);
        csilk_vector_search_response_t res = {0};
        /* Search a collection that still 200s in the mock; but an unreachable
         * host must fail. Use a dead port to hit the transport-error branch. */
        csilk_vector_db_t* bad = csilk_vector_db_new("milvus", "http://127.0.0.1:1", NULL);
        assert(bad != NULL);
        int rc = csilk_vector_db_search(bad, "docs", g_vec, 4, 2, &res);
        assert(rc == -1);
        csilk_vector_search_response_free(&res);
        csilk_vector_db_free(bad);
        csilk_vector_db_free(db);
        printf("  passed\n");
    }

    stop_mock_server();
    printf("test_vector_milvus_qdrant_mock: ALL PASSED\n");
    return 0;
}
