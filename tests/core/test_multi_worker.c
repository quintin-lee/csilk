#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define MW_PORT 8099
#define NUM_CLIENTS 8
#define NUM_WORKERS 3

static volatile int    server_ready = 0;
static csilk_server_t* g_server = nullptr;

static void
ping_handler(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "pong");
}

static void
on_server_start(csilk_ctx_t* c)
{
    (void)c;
    server_ready = 1;
}

static void*
run_server(void* arg)
{
    (void)arg;
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h[] = {ping_handler};
    csilk_router_add(router, "GET", "/ping", h, 1);

    g_server = csilk_server_new(router);
    csilk_server_config_t cfg = {.idle_timeout_ms = 500,
                                 .max_body_size = 1048576,
                                 .max_header_size = 65536,
                                 .listen_backlog = 128,
                                 .worker_threads = NUM_WORKERS};
    csilk_server_set_config(g_server, &cfg);

    csilk_server_add_hook(g_server, CSILK_HOOK_SERVER_START, on_server_start);

    csilk_server_run(g_server, MW_PORT);
    csilk_server_free(g_server);
    csilk_router_free(router);
    return nullptr;
}

typedef struct {
    int id;
    int success_count;
    int fail_count;
} client_result_t;

static void*
run_client(void* arg)
{
    client_result_t* r = (client_result_t*)arg;
    r->success_count = 0;
    r->fail_count = 0;

    for (int i = 0; i < 5; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            r->fail_count++;
            continue;
        }
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(MW_PORT);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            r->fail_count++;
            continue;
        }
        const char* req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send(sock, req, strlen(req), 0);

        fd_set         fds;
        struct timeval tv = {10, 0};
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        char buf[1024] = {0};
        int  ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0) {
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "200 OK") && strstr(buf, "pong")) {
                    r->success_count++;
                } else {
                    r->fail_count++;
                    printf("FAIL (wrong response): %s\n", buf);
                }
            } else {
                r->fail_count++;
                printf("FAIL (recv returned %d, errno %d)\n", n, errno);
            }
        } else {
            r->fail_count++;
            printf("FAIL (select returned %d, errno %d)\n", ret, errno);
        }
        close(sock);
    }
    return nullptr;
}

int
main(void)
{
    csilk_log_config_t lcfg = {0};
    csilk_log_init(lcfg);
    printf("TEST STARTING\n");
    pthread_t srv;
    pthread_create(&srv, nullptr, run_server, nullptr);

    int retries = 0;
    while (!server_ready && retries < 50) {
        usleep(100000);
        retries++;
    }
    if (!server_ready) {
        printf("FAIL: server failed to start\n");
        pthread_join(srv, nullptr);
        return 1;
    }
    usleep(100000);

    pthread_t       clients[NUM_CLIENTS];
    client_result_t results[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        results[i].id = i;
        pthread_create(&clients[i], nullptr, run_client, &results[i]);
    }

    int total_success = 0;
    int total_fail = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(clients[i], nullptr);
        total_success += results[i].success_count;
        total_fail += results[i].fail_count;
    }

    csilk_server_stop(g_server);
    pthread_join(srv, nullptr);

    int expected = NUM_CLIENTS * 5;
    printf("multi_worker: %d/%d success, %d failed\n", total_success, expected, total_fail);

    if (total_success == expected && total_fail == 0) {
        printf("PASS: multi_worker test\n");
        return 0;
    }
    printf("FAIL: expected %d/5 per client, got %d success %d fail\n",
           expected,
           total_success,
           total_fail);
    return 1;
}
