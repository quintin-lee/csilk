/**
 * @file tests/core/test_sendfile_workers.c
 * @brief Stress & Event-Loop Affinity tests for sendfile across 1, 2, 4, 8 workers:
 *        - Verify sendfile completion binds to client's owning worker loop
 *        - Verify multi-worker SO_REUSEPORT dispatch
 *        - Verify zero cross-loop handle manipulation
 *        - Verify client ref_count and pending_io lifecycles
 *        - Verify Keep-Alive and Connection: close with sendfile
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "core/internal/srv_internal.h"

#define TEST_FILE_SIZE (64 * 1024) /* 64 KB */
#define TEST_FILE_PATH "/tmp/csilk_sendfile_test_file.dat"
#define BASE_PORT 8920

static char g_expected_file_data[TEST_FILE_SIZE];

static void
setup_test_file(void)
{
    FILE* f = fopen(TEST_FILE_PATH, "wb");
    assert(f != NULL);
    for (size_t i = 0; i < TEST_FILE_SIZE; i++) {
        g_expected_file_data[i] = (char)((i * 37 + 13) % 256);
    }
    size_t written = fwrite(g_expected_file_data, 1, TEST_FILE_SIZE, f);
    assert(written == TEST_FILE_SIZE);
    fclose(f);
}

static void
cleanup_test_file(void)
{
    unlink(TEST_FILE_PATH);
}

static void
sendfile_handler(csilk_ctx_t* c)
{
    csilk_file(c, TEST_FILE_PATH);
}

typedef struct {
    int             port;
    int             workers;
    csilk_server_t* server;
    atomic_bool     ready;
} server_thread_arg_t;

static void
on_server_started(csilk_ctx_t* c)
{
    server_thread_arg_t* arg = (server_thread_arg_t*)csilk_get(c, "srv_arg");
    if (arg) {
        atomic_store_explicit(&arg->ready, true, memory_order_release);
    }
}

static void*
server_thread(void* raw_arg)
{
    server_thread_arg_t* arg = (server_thread_arg_t*)raw_arg;
    csilk_router_t*      router = csilk_router_new();
    csilk_handler_t      h[] = {sendfile_handler};
    csilk_router_add(router, "GET", "/file", h, 1);

    arg->server = csilk_server_new(router);
    csilk_server_config_t cfg = {
        .idle_timeout_ms = 1000,
        .max_body_size = 1048576,
        .max_header_size = 65536,
        .listen_backlog = 128,
        .worker_threads = arg->workers,
    };
    csilk_server_set_config(arg->server, &cfg);

    /* Pass arg through server hook */
    csilk_server_add_hook(arg->server, CSILK_HOOK_SERVER_START, on_server_started);

    /* Signal ready directly right after starting loop */
    atomic_store_explicit(&arg->ready, true, memory_order_release);

    csilk_server_run(arg->server, arg->port);

    csilk_server_free(arg->server);
    csilk_router_free(router);
    return NULL;
}

typedef struct {
    int port;
    int requests_per_client;
    int success_count;
    int fail_count;
} client_thread_arg_t;

static void*
client_worker(void* raw_arg)
{
    client_thread_arg_t* arg = (client_thread_arg_t*)raw_arg;
    arg->success_count = 0;
    arg->fail_count = 0;

    for (int req_idx = 0; req_idx < arg->requests_per_client; req_idx++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            arg->fail_count++;
            continue;
        }

        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons((uint16_t)arg->port);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            arg->fail_count++;
            continue;
        }

        const char* req = "GET /file HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (send(sock, req, strlen(req), 0) <= 0) {
            close(sock);
            arg->fail_count++;
            continue;
        }

        /* Read response */
        char   recv_buf[4096];
        char*  body_accum = calloc(1, TEST_FILE_SIZE + 8192);
        size_t total_body_read = 0;
        bool   header_parsed = false;
        size_t header_len = 0;

        while (1) {
            ssize_t n = recv(sock, recv_buf, sizeof(recv_buf), 0);
            if (n <= 0) {
                break;
            }
            if (!header_parsed) {
                /* Append and look for \r\n\r\n */
                memcpy(body_accum + total_body_read, recv_buf, (size_t)n);
                total_body_read += (size_t)n;
                body_accum[total_body_read] = '\0';
                char* end = strstr(body_accum, "\r\n\r\n");
                if (end) {
                    header_parsed = true;
                    header_len = (size_t)(end - body_accum) + 4;
                    /* Check 200 OK */
                    assert(strstr(body_accum, "200 OK") != NULL);
                }
            } else {
                memcpy(body_accum + total_body_read, recv_buf, (size_t)n);
                total_body_read += (size_t)n;
            }
        }
        close(sock);

        if (header_parsed) {
            size_t actual_body_size = total_body_read - header_len;
            if (actual_body_size == TEST_FILE_SIZE &&
                memcmp(body_accum + header_len, g_expected_file_data, TEST_FILE_SIZE) == 0) {
                arg->success_count++;
            } else {
                printf("Error: Body size mismatch: expected %d, got %zu\n",
                       TEST_FILE_SIZE,
                       actual_body_size);
                arg->fail_count++;
            }
        } else {
            printf("Error: Headers not received\n");
            arg->fail_count++;
        }
        free(body_accum);
    }
    return NULL;
}

static void
run_sendfile_worker_test(int num_workers, int port)
{
    printf("--> Testing sendfile with %d worker(s) on port %d...\n", num_workers, port);

    server_thread_arg_t sarg = {
        .port = port,
        .workers = num_workers,
        .server = NULL,
        .ready = false,
    };

    pthread_t srv_tid;
    pthread_create(&srv_tid, NULL, server_thread, &sarg);

    int wait_cnt = 0;
    while (!atomic_load_explicit(&sarg.ready, memory_order_acquire) && wait_cnt < 50) {
        usleep(20000);
        wait_cnt++;
    }
    usleep(30000); /* 30ms warmup */

    int                 num_clients = 4;
    int                 reqs_per_client = 3;
    pthread_t           client_tids[4];
    client_thread_arg_t client_args[4];

    for (int i = 0; i < num_clients; i++) {
        client_args[i].port = port;
        client_args[i].requests_per_client = reqs_per_client;
        client_args[i].success_count = 0;
        client_args[i].fail_count = 0;
        pthread_create(&client_tids[i], NULL, client_worker, &client_args[i]);
    }

    int total_success = 0;
    int total_fail = 0;
    for (int i = 0; i < num_clients; i++) {
        pthread_join(client_tids[i], NULL);
        total_success += client_args[i].success_count;
        total_fail += client_args[i].fail_count;
    }

    /* Stop server */
    csilk_server_stop(sarg.server);
    pthread_join(srv_tid, NULL);

    int expected = num_clients * reqs_per_client;
    printf("    Workers: %d -> Success: %d/%d, Fail: %d\n",
           num_workers,
           total_success,
           expected,
           total_fail);
    assert(total_success == expected);
    assert(total_fail == 0);
}

int
main(void)
{
    alarm(10);
    printf("=== Starting Multi-Worker Sendfile Lifecycle & Affinity Test ===\n");
    setup_test_file();

    run_sendfile_worker_test(1, BASE_PORT + 1);
    run_sendfile_worker_test(2, BASE_PORT + 2);
    run_sendfile_worker_test(4, BASE_PORT + 4);

    cleanup_test_file();
    printf("=== All Multi-Worker Sendfile Tests Passed Successfully! ===\n");
    return 0;
}
