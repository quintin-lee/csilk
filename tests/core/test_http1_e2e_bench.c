/**
 * @file test_http1_e2e_bench.c
 * @brief Real TCP HTTP/1.1 end-to-end latency benchmark.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "csilk/csilk.h"

#define E2E_BASE_PORT 18991
#define E2E_REQUESTS 1000
#define E2E_TIMEOUT_SEC 5

static volatile int    server_ready;
static csilk_server_t* server;

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "ok");
}

typedef struct {
    int port;
    int workers;
} server_args_t;

static void*
server_main(void* raw_args)
{
    server_args_t*  args = raw_args;
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {handler};
    assert(router != NULL);
    assert(csilk_router_add(router, "GET", "/health", handlers, 1) == 0);
    server = csilk_server_new(router);
    assert(server != NULL);
    csilk_server_config_t config = {
        .worker_threads = args->workers,
        .idle_timeout_ms = 5000,
        .max_body_size = 1024 * 1024,
        .max_header_size = 64 * 1024,
        .listen_backlog = 128,
    };
    csilk_server_set_config(server, &config);
    server_ready = 1;
    csilk_server_run(server, args->port);
    csilk_server_free(server);
    csilk_router_free(router);
    return NULL;
}

static int
connect_client(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval timeout = {.tv_sec = E2E_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
read_response(int fd)
{
    char   buffer[4096];
    size_t used = 0;
    while (used < sizeof(buffer) - 1) {
        ssize_t n = recv(fd, buffer + used, sizeof(buffer) - 1 - used, 0);
        if (n <= 0) {
            return -1;
        }
        used += (size_t)n;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            return strstr(buffer, "200 OK") != NULL ? 0 : -1;
        }
    }
    return -1;
}

static int
compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}

static void
run_worker_benchmark(int workers, int port)
{
    server_ready = 0;
    server = NULL;
    server_args_t args = {.port = port, .workers = workers};
    pthread_t     thread;
    assert(pthread_create(&thread, NULL, server_main, &args) == 0);
    for (int i = 0; i < E2E_TIMEOUT_SEC * 100 && !server_ready; i++) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
    assert(server_ready);

    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        fd = connect_client(port);
        if (fd < 0) {
            usleep(10000);
        }
    }
    assert(fd >= 0);

    static const char request[] = "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                                  "Connection: keep-alive\r\n\r\n";
    uint64_t          latencies[E2E_REQUESTS];
    size_t            completed = 0;
    uint64_t          start_all = now_ns();
    for (size_t i = 0; i < E2E_REQUESTS; i++) {
        uint64_t start = now_ns();
        if (send(fd, request, sizeof(request) - 1, 0) != (ssize_t)(sizeof(request) - 1) ||
            read_response(fd) != 0) {
            break;
        }
        latencies[completed++] = now_ns() - start;
    }
    uint64_t elapsed = now_ns() - start_all;
    close(fd);
    csilk_server_stop(server);
    pthread_join(thread, NULL);

    assert(completed == E2E_REQUESTS);
    qsort(latencies, completed, sizeof(latencies[0]), compare_u64);
    printf("HTTP1_E2E workers=%d requests=%zu qps=%.2f p50_ns=%" PRIu64 " p95_ns=%" PRIu64
           " p99_ns=%" PRIu64 " errors=%zu\n",
           workers,
           completed,
           (double)completed * 1000000000.0 / (double)elapsed,
           latencies[completed * 50 / 100],
           latencies[completed * 95 / 100],
           latencies[completed * 99 / 100],
           E2E_REQUESTS - completed);
}

int
main(void)
{
    alarm(60);
    const int worker_counts[] = {1, 2, 4, 8};
    for (size_t i = 0; i < sizeof(worker_counts) / sizeof(worker_counts[0]); i++) {
        run_worker_benchmark(worker_counts[i], E2E_BASE_PORT + (int)i);
    }
    return 0;
}
