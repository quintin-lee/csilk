#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/internal/srv_internal.h"
#include "csilk/csilk.h"

static int            dummy_handler_called;
static _Atomic(bool)* g_spa_ready_ptr = nullptr;

static void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
    dummy_handler_called++;
}

typedef struct {
    csilk_server_t* server;
    int             port;
    atomic_bool     ready;
} spa_srv_arg_t;

static void
on_spa_srv_started(csilk_ctx_t* c)
{
    (void)c;
    if (g_spa_ready_ptr) {
        atomic_store_explicit(g_spa_ready_ptr, true, memory_order_release);
    }
}

static void*
spa_srv_thread(void* raw)
{
    spa_srv_arg_t* arg = (spa_srv_arg_t*)raw;
    g_spa_ready_ptr = &arg->ready;
    csilk_server_add_hook(arg->server, CSILK_HOOK_SERVER_START, on_spa_srv_started);
    csilk_server_run(arg->server, arg->port);
    return nullptr;
}

static int
spa_http_exchange(int port, const char* raw_req, char* resp_buf, size_t resp_cap)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((uint16_t)port);
    assert(connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    size_t req_len = strlen(raw_req);
    assert(send(sock, raw_req, req_len, 0) == (ssize_t)req_len);

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = recv(sock, resp_buf + total, resp_cap - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    resp_buf[total] = '\0';
    close(sock);
    return (int)total;
}

static void
test_spa_fallback_functional(void)
{
    printf("Testing SPA fallback pre-read cache, async miss, and path safety...\n");

    /* 1. Traversal / control character rejection */
    {
        csilk_router_t* r = csilk_router_new();
        csilk_server_t* srv = csilk_server_new(r);
        csilk_server_set_spa_fallback(srv, "/tmp/../etc");
        assert(srv->spa_doc_root == nullptr);
        assert(srv->spa_index_buf == nullptr);
        csilk_server_set_spa_fallback(srv, "/tmp/bad\npath");
        assert(srv->spa_doc_root == nullptr);
        csilk_server_free(srv);
        csilk_router_free(r);
    }

    /* 2. Pre-read cache hit (file unlinked before server starts) */
    {
        char  dir_tmpl[] = "/tmp/csilk_spa_cached_XXXXXX";
        char* dir = mkdtemp(dir_tmpl);
        assert(dir != nullptr);

        char index_path[512];
        snprintf(index_path, sizeof(index_path), "%s/index.html", dir);
        FILE* f = fopen(index_path, "wb");
        assert(f != nullptr);
        const char* payload = "<html>spa-cached</html>";
        assert(fwrite(payload, 1, strlen(payload), f) == strlen(payload));
        fclose(f);

        csilk_router_t* r = csilk_router_new();
        csilk_server_t* srv = csilk_server_new(r);
        csilk_server_set_spa_fallback(srv, dir);
        assert(srv->spa_index_buf != nullptr);
        assert(srv->spa_index_len == strlen(payload));

        /* Delete disk file so only the in-memory pre-read cache can satisfy GET */
        assert(unlink(index_path) == 0);

        spa_srv_arg_t arg = {.server = srv, .port = 19841, .ready = false};
        pthread_t     tid;
        assert(pthread_create(&tid, nullptr, spa_srv_thread, &arg) == 0);
        for (int i = 0; i < 100 && !atomic_load_explicit(&arg.ready, memory_order_acquire); i++) {
            usleep(10000);
        }
        assert(atomic_load_explicit(&arg.ready, memory_order_acquire));

        char resp[4096];
        spa_http_exchange(
            19841,
            "GET /client/route HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
            resp,
            sizeof(resp));
        assert(strstr(resp, "200 OK") != nullptr);
        assert(strstr(resp, "text/html") != nullptr);
        assert(strstr(resp, payload) != nullptr);

        spa_http_exchange(19841,
                          "POST /client/route HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "
                          "0\r\nConnection: close\r\n\r\n",
                          resp,
                          sizeof(resp));
        assert(strstr(resp, "404 Not Found") != nullptr);

        csilk_server_stop(srv);
        pthread_join(tid, nullptr);
        csilk_server_free(srv);
        csilk_router_free(r);
        rmdir(dir);
    }

    /* 3. Cache-miss async fallback (index.html created after server start) */
    {
        char  dir_tmpl[] = "/tmp/csilk_spa_async_XXXXXX";
        char* dir = mkdtemp(dir_tmpl);
        assert(dir != nullptr);

        csilk_router_t* r = csilk_router_new();
        csilk_server_t* srv = csilk_server_new(r);
        csilk_server_set_spa_fallback(srv, dir);
        assert(srv->spa_doc_root != nullptr);
        assert(srv->spa_index_buf == nullptr);

        spa_srv_arg_t arg = {.server = srv, .port = 19842, .ready = false};
        pthread_t     tid;
        assert(pthread_create(&tid, nullptr, spa_srv_thread, &arg) == 0);
        for (int i = 0; i < 100 && !atomic_load_explicit(&arg.ready, memory_order_acquire); i++) {
            usleep(10000);
        }
        assert(atomic_load_explicit(&arg.ready, memory_order_acquire));

        char index_path[512];
        snprintf(index_path, sizeof(index_path), "%s/index.html", dir);
        FILE* f = fopen(index_path, "wb");
        assert(f != nullptr);
        const char* payload = "<html>spa-async-miss</html>";
        assert(fwrite(payload, 1, strlen(payload), f) == strlen(payload));
        fclose(f);

        char resp[4096];
        spa_http_exchange(
            19842,
            "GET /late/route HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
            resp,
            sizeof(resp));
        assert(strstr(resp, "200 OK") != nullptr);
        assert(strstr(resp, "text/html") != nullptr);
        assert(strstr(resp, payload) != nullptr);

        csilk_server_stop(srv);
        pthread_join(tid, nullptr);
        csilk_server_free(srv);
        csilk_router_free(r);
        unlink(index_path);
        rmdir(dir);
    }
}

int
main()
{
    csilk_router_t* r = csilk_router_new();
    csilk_server_t* srv = csilk_server_new(r);
    assert(srv != nullptr);

    printf("Testing csilk_server_set_not_found_handler...\n");
    {
        dummy_handler_called = 0;
        csilk_server_set_not_found_handler(srv, dummy_handler);
        csilk_server_set_not_found_handler(nullptr, dummy_handler);
        csilk_server_set_not_found_handler(srv, nullptr);
    }

    printf("Testing csilk_server_set_spa_fallback...\n");
    {
        csilk_server_set_spa_fallback(srv, "/tmp/static");
        csilk_server_set_spa_fallback(nullptr, "/tmp/static");
        csilk_server_set_spa_fallback(srv, nullptr);
        csilk_server_set_spa_fallback(srv, "/tmp/other");
    }

    printf("Testing csilk_server_set_max_connections...\n");
    {
        int prev = csilk_server_set_max_connections(srv, 100);
        assert(prev >= 0);
        prev = csilk_server_set_max_connections(srv, 0);
        assert(prev == 100);
        prev = csilk_server_set_max_connections(nullptr, 50);
        assert(prev == -1);
    }

    printf("Testing csilk_server_set_storage_driver...\n");
    {
        csilk_server_set_storage_driver(srv, nullptr);
        csilk_server_set_storage_driver(nullptr, nullptr);
    }

    printf("Testing csilk_server_set_crypto_driver...\n");
    {
        csilk_server_set_crypto_driver(srv, nullptr);
        csilk_server_set_crypto_driver(nullptr, nullptr);
    }

    printf("Testing nullptr safety for all setters...\n");
    {
        csilk_server_set_not_found_handler(nullptr, dummy_handler);
        csilk_server_set_spa_fallback(nullptr, "/tmp/x");
        csilk_server_set_storage_driver(nullptr, nullptr);
        csilk_server_set_crypto_driver(nullptr, nullptr);
    }

    csilk_server_free(srv);
    csilk_router_free(r);

    test_spa_fallback_functional();

    printf("test_server_ext: PASS\n");
    return 0;
}
