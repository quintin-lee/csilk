/**
 * @file test_server_bind_fail_recover.c
 * @brief Regression: a failed csilk_server_run() (bind error) must fully
 *        release the shared default event loop.
 *
 * Old behavior: the bind-failure path in csilk_server_run() returned -1
 * without closing server->async_handle, and csilk_server_free() never
 * closed it either. The stale async handle (carrying a freed server
 * pointer) stayed registered on the shared default loop; the NEXT server
 * to run that loop dispatched the stale callback -> use-after-free.
 *
 * This test forces a bind failure, tears the server down, then starts a
 * fresh server on the same default loop and serves a real request. Under
 * ASAN the old code aborts in the second run.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/csilk.h"

/* --- small HTTP client helpers (mirror test_server_limits.c) --- */

static int
bind_ephemeral_and_get_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int                yes = 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* ephemeral */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    /* The socket stays open so this port is guaranteed occupied. */
    (void)port;
    return fd;
}

static int
connect_to_port(int port)
{
    for (int retry = 0; retry < 100; retry++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return -1;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons((uint16_t)port);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return sock;
        }
        close(sock);
        usleep(10000);
    }
    return -1;
}

/* --- server driver thread --- */

static csilk_server_t* g_server;
static int             g_run_result = -999;
static int             g_port;

static void*
run_server_thread(void* unused)
{
    (void)unused;
    g_run_result = csilk_server_run(g_server, g_port);
    return nullptr;
}

static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "hello-world");
}

int
main(void)
{
    /* 1. Occupy a port so csilk_server_run() is forced to fail binding. */
    int occupier = bind_ephemeral_and_get_port();
    assert(occupier >= 0);
    struct sockaddr_in addr;
    socklen_t          alen = sizeof(addr);
    getsockname(occupier, (struct sockaddr*)&addr, &alen);
    int busy_port = ntohs(addr.sin_port);
    assert(busy_port > 0);

    /* 2. First server: run must fail cleanly on the busy port. */
    {
        csilk_router_t* r = csilk_router_new();
        assert(r != nullptr);
        csilk_server_t* srv = csilk_server_new(r);
        assert(srv != nullptr);

        g_server = srv;
        g_port = busy_port;
        pthread_t th;
        assert(pthread_create(&th, nullptr, run_server_thread, nullptr) == 0);
        pthread_join(th, nullptr);
        assert(g_run_result < 0 && "expected bind failure");

        /* Reproduce the Python `App.stop()` + `App.free()` call order. */
        csilk_server_stop(srv);
        csilk_server_free(srv);
        csilk_router_free(r);
    }

    /* 3. Release the occupied port, then run a FRESH server on the same
     *    shared default event loop; the stale async handle (old code) fires
     *    here -> use-after-free (ASAN abort). */
    close(occupier);

    int free_port = -1;
    {
        int probe = bind_ephemeral_and_get_port();
        assert(probe >= 0);
        socklen_t plen = sizeof(addr);
        getsockname(probe, (struct sockaddr*)&addr, &plen);
        free_port = ntohs(addr.sin_port);
        close(probe);
        assert(free_port > 0);
    }

    csilk_router_t* r2 = csilk_router_new();
    assert(r2 != nullptr);
    csilk_server_t* srv2 = csilk_server_new(r2);
    assert(srv2 != nullptr);
    assert(csilk_router_add(r2, "GET", "/hello", (csilk_handler_t[]){hello_handler, NULL}, 1) == 0);

    g_server = srv2;
    g_port = free_port;
    pthread_t th2;
    assert(pthread_create(&th2, nullptr, run_server_thread, nullptr) == 0);

    int sock = connect_to_port(free_port);
    assert(sock >= 0 && "second server did not come up after failed-bind teardown");

    const char* req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    assert(send(sock, req, strlen(req), 0) == (ssize_t)strlen(req));
    char buf[1024];
    int  n = recv(sock, buf, sizeof(buf) - 1, 0);
    assert(n > 0);
    buf[n] = '\0';
    assert(strstr(buf, "200 OK") != nullptr);
    assert(strstr(buf, "hello-world") != nullptr);
    close(sock);

    csilk_server_stop(srv2);
    /* Startup must have succeeded; the loop exit code is either 0 (drained)
     * or 1 (uv_stop with pending close callbacks) — never a negative
     * startup error. */
    assert(g_run_result <= 0);
    pthread_join(th2, nullptr);
    csilk_server_free(srv2);
    csilk_router_free(r2);

    printf("test_server_bind_fail_recover: PASS\n");
    return 0;
}