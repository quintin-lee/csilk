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
#define E2E_CLIENTS 4
#define E2E_REQUESTS_PER_CLIENT 250
#define E2E_ROUNDS 3
#define E2E_TIMEOUT_SEC 5
#define E2E_TOTAL_SAMPLES (E2E_CLIENTS * E2E_REQUESTS_PER_CLIENT * E2E_ROUNDS)

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

typedef struct {
    int       port;
    size_t    requests;
    uint64_t* samples;
    size_t    sample_offset;
    size_t    completed;
} client_args_t;

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

static void*
client_main(void* raw_args)
{
    client_args_t* args = raw_args;
    int            fd = connect_client(args->port);
    if (fd < 0) {
        return NULL;
    }
    static const char request[] = "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                                  "Connection: keep-alive\r\n\r\n";
    for (size_t i = 0; i < args->requests; i++) {
        uint64_t start = now_ns();
        if (send(fd, request, sizeof(request) - 1, 0) != (ssize_t)(sizeof(request) - 1) ||
            read_response(fd) != 0) {
            break;
        }
        args->samples[args->sample_offset + args->completed++] = now_ns() - start;
    }
    close(fd);
    return NULL;
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
    server_args_t server_args = {.port = port, .workers = workers};
    pthread_t     server_thread;
    assert(pthread_create(&server_thread, NULL, server_main, &server_args) == 0);
    for (int i = 0; i < E2E_TIMEOUT_SEC * 100 && !server_ready; i++) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
    assert(server_ready);

    uint64_t* samples = calloc(E2E_TOTAL_SAMPLES, sizeof(*samples));
    assert(samples != NULL);
    size_t   completed = 0;
    uint64_t start_all = now_ns();
    for (int round = 0; round < E2E_ROUNDS; round++) {
        pthread_t     clients[E2E_CLIENTS];
        client_args_t client_args[E2E_CLIENTS];
        size_t        round_offset = (size_t)round * E2E_CLIENTS * E2E_REQUESTS_PER_CLIENT;
        for (int i = 0; i < E2E_CLIENTS; i++) {
            client_args[i] = (client_args_t){
                .port = port,
                .requests = E2E_REQUESTS_PER_CLIENT,
                .samples = samples,
                .sample_offset = round_offset + (size_t)i * E2E_REQUESTS_PER_CLIENT,
                .completed = 0,
            };
            assert(pthread_create(&clients[i], NULL, client_main, &client_args[i]) == 0);
        }
        for (int i = 0; i < E2E_CLIENTS; i++) {
            pthread_join(clients[i], NULL);
            completed += client_args[i].completed;
        }
    }
    uint64_t elapsed = now_ns() - start_all;
    csilk_server_stop(server);
    pthread_join(server_thread, NULL);

    assert(completed == E2E_TOTAL_SAMPLES);
    qsort(samples, completed, sizeof(samples[0]), compare_u64);
    printf("HTTP1_E2E workers=%d clients=%d rounds=%d requests=%zu qps=%.2f p50_ns=%" PRIu64
           " p95_ns=%" PRIu64 " p99_ns=%" PRIu64 " errors=%zu\n",
           workers,
           E2E_CLIENTS,
           E2E_ROUNDS,
           completed,
           (double)completed * 1000000000.0 / (double)elapsed,
           samples[completed * 50 / 100],
           samples[completed * 95 / 100],
           samples[completed * 99 / 100],
           E2E_TOTAL_SAMPLES - completed);
    free(samples);
}

int
main(void)
{
    alarm(120);
    const int worker_counts[] = {1, 2, 4, 8};
    for (size_t i = 0; i < sizeof(worker_counts) / sizeof(worker_counts[0]); i++) {
        run_worker_benchmark(worker_counts[i], E2E_BASE_PORT + (int)i);
    }
    return 0;
}
