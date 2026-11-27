/**
 * @file test_admin.c
 * @brief Tests for the admin dashboard module.
 * @copyright MIT License
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/app/admin.h"
#include "csilk/app/app.h"
#include "csilk/core/json/json.h"
#include "csilk/core/sync.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/util/flamegraph.h"
#ifdef CSILK_HAS_DB
#include "csilk/drivers/db.h"
#endif
#ifdef CSILK_HAS_AI
#include "csilk/drivers/ai.h"
#endif

static int tests_run = 0;
static int tests_passed = 0;

#ifdef CSILK_HAS_AI
static void*
admin_mock_ai_init(const char* api_key, const char* base_url)
{
    (void)api_key;
    (void)base_url;
    return (void*)0x1;
}

static int
admin_mock_ai_chat(void* state, const csilk_ai_chat_request_t* req, csilk_ai_chat_response_t* res)
{
    (void)state;
    (void)req;
    res->content = strdup("ok");
    res->prompt_tokens = 2;
    res->completion_tokens = 3;
    res->total_tokens = 5;
    return 0;
}

static void
admin_mock_ai_free(void* state)
{
    (void)state;
}
#endif

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
    } while (0)

static csilk_ctx_t*
make_ctx(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    return c;
}

static void
free_ctx(csilk_ctx_t* c)
{
    csilk_test_ctx_free(c);
}

static void
test_admin_enable_metrics_collection(void)
{
    csilk_ctx_t* c = make_ctx();
    csilk_set(c, "_admin_metrics_enabled", (void*)(uintptr_t)1);
    void* v = csilk_get(c, "_admin_metrics_enabled");
    if ((int)(uintptr_t)v == 1) {
        PASS();
    } else {
        FAIL("admin metrics flag not set");
    }
    free_ctx(c);
}

static void
test_admin_stats_collection(void)
{
    csilk_ctx_t* c = make_ctx();

    csilk_set(c, "admin_total_requests", (void*)(uintptr_t)42);
    csilk_set(c, "admin_active_connections", (void*)(uintptr_t)7);

    void* reqs = csilk_get(c, "admin_total_requests");
    void* conns = csilk_get(c, "admin_active_connections");

    if ((int)(uintptr_t)reqs == 42 && (int)(uintptr_t)conns == 7) {
        PASS();
    } else {
        FAIL("admin stats values incorrect");
    }
    free_ctx(c);
}

static void
test_admin_ctx_storage_overflow(void)
{
    csilk_ctx_t* c = make_ctx();
    int          ok = 1;

    for (int i = 0; i < CSILK_MAX_STORAGE + 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "admin_k%d", i);
        csilk_set(c, key, (void*)(uintptr_t)(i + 1));
    }

    for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
        char key[16];
        snprintf(key, sizeof(key), "admin_k%d", i);
        void* val = csilk_get(c, key);
        if (val == nullptr) {
            ok = 0;
            break;
        }
    }

    if (ok) {
        PASS();
    } else {
        FAIL("storage overflow lost keys");
    }
    free_ctx(c);
}

static void
dummy_admin_auth_mw(csilk_ctx_t* c)
{
    csilk_set(c, "admin_authenticated", (void*)(uintptr_t)1);
    csilk_next(c);
}

static void
test_admin_serve_requires_auth(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    /* Unauthenticated csilk_admin_serve must refuse to register routes */
    csilk_admin_serve(app, "/admin");

    csilk_ctx_t* c = make_ctx();
    csilk_test_ctx_set_request(c, "GET", "/admin/stats");
    int matched = csilk_router_match_ctx(csilk_app_router(app), c);
    if (matched == 0) {
        PASS();
    } else {
        FAIL("csilk_admin_serve should not register unauthenticated routes");
    }
    free_ctx(c);
    csilk_app_free(app);
}

static void
test_admin_serve_secure_and_stats_endpoint(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    csilk_ctx_t* c = make_ctx();
    csilk_test_ctx_set_request(c, "GET", "/admin/stats");
    int matched = csilk_router_match_ctx(csilk_app_router(app), c);
    if (matched == 1) {
        csilk_next(c);
    }
    if (matched == 1 && csilk_get_status(c) == 200 &&
        (int)(uintptr_t)csilk_get(c, "admin_authenticated") == 1) {
        size_t        body_len = 0;
        const char*   body = csilk_get_response_body(c, &body_len);
        csilk_json_t* js = body ? csilk_json_parse_len(body, body_len) : nullptr;
        csilk_json_t* mq_js = js ? csilk_json_get(js, "mq") : nullptr;
        csilk_json_t* ai_js = js ? csilk_json_get(js, "ai") : nullptr;
        csilk_json_t* db_js = js ? csilk_json_get(js, "db") : nullptr;
        if (js && csilk_json_get(js, "total_requests") && mq_js &&
            csilk_json_get(mq_js, "published") && csilk_json_get(mq_js, "delivered") &&
            csilk_json_get(mq_js, "depth") && ai_js && csilk_json_get(ai_js, "requests") &&
            csilk_json_get(ai_js, "tokens") && csilk_json_get(ai_js, "errors") &&
            csilk_json_get(ai_js, "avg_duration") && db_js && csilk_json_get(db_js, "queries") &&
            csilk_json_get(db_js, "execs") && csilk_json_get(db_js, "errors") &&
            csilk_json_get(db_js, "avg_duration") && csilk_json_get(js, "sys") &&
            csilk_json_get(js, "security") && csilk_json_get(js, "process")) {
            PASS();
        } else {
            FAIL("admin /stats response missing expected JSON fields");
        }
        if (js) {
            csilk_json_free(js);
        }
    } else {
        FAIL("admin /stats route did not execute or return 200");
    }
    free_ctx(c);
    csilk_app_free(app);
}

static void
test_admin_stats_reflects_optional_subsystems(void)
{
#ifdef CSILK_HAS_DB
    csilk_db_init();
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != nullptr);
    assert(csilk_db_exec(pool, "CREATE TABLE admin_t (id INTEGER);") == 0);
    csilk_db_pool_free(pool);
#endif

#ifdef CSILK_HAS_AI
    static const csilk_ai_driver_t mock_driver = {
        .name = "admin_mock",
        .init = admin_mock_ai_init,
        .chat = admin_mock_ai_chat,
        .embeddings = nullptr,
        .free = admin_mock_ai_free,
    };
    csilk_ai_register_driver(&mock_driver);
    csilk_ai_t* ai = csilk_ai_new("admin_mock", "k", nullptr);
    assert(ai != nullptr);
    csilk_ai_chat_request_t  req = {.model = "m", .user = "hi"};
    csilk_ai_chat_response_t res = {0};
    assert(csilk_ai_chat(ai, &req, &res) == 0);
    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
#endif

    csilk_app_t* app = csilk_app_new(nullptr);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    csilk_ctx_t* c = make_ctx();
    csilk_test_ctx_set_request(c, "GET", "/admin/stats");
    int matched = csilk_router_match_ctx(csilk_app_router(app), c);
    assert(matched == 1);
    csilk_next(c);
    assert(csilk_get_status(c) == 200);

    size_t        body_len = 0;
    const char*   body = csilk_get_response_body(c, &body_len);
    csilk_json_t* js = body ? csilk_json_parse_len(body, body_len) : nullptr;
    assert(js != nullptr);

    int ok = 1;
#ifdef CSILK_HAS_DB
    csilk_json_t* db_js = csilk_json_get(js, "db");
    if (!db_js || csilk_json_get_number(db_js, "execs") < 1.0) {
        ok = 0;
    }
#endif
#ifdef CSILK_HAS_AI
    csilk_json_t* ai_js = csilk_json_get(js, "ai");
    if (!ai_js || csilk_json_get_number(ai_js, "requests") < 1.0) {
        ok = 0;
    }
#endif
    if (ok) {
        PASS();
    } else {
        FAIL("admin /stats did not reflect active DB/AI telemetry");
    }

    csilk_json_free(js);
    free_ctx(c);
    csilk_app_free(app);
}

typedef struct {
    csilk_app_t* app;
    int          port;
    atomic_int   ready;
} admin_test_srv_t;

static void
admin_test_srv_thread(void* arg)
{
    admin_test_srv_t* srv = (admin_test_srv_t*)arg;
    atomic_store(&srv->ready, 1);
    csilk_app_run(srv->app, srv->port);
}

static int
admin_pick_ephemeral_port(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int port = -1;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    close(sock);
    return port;
}

static int
admin_connect_localhost(int port)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return -1;
        }
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return sock;
        }
        close(sock);
        usleep(10000);
    }
    return -1;
}

static void
test_admin_profile_async_response(void)
{
    int port = admin_pick_ephemeral_port();
    assert(port > 0);

    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);
    csilk_config_t*       full_cfg = csilk_app_config(app);
    csilk_server_config_t cfg = full_cfg->server;
    free(full_cfg);
    /* Keep single worker for lightweight unit test */
    cfg.worker_threads = 1;
    csilk_app_set_server_config(app, cfg);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    admin_test_srv_t srv = {app, port, 0};
    csilk_thread_t   tid;
    assert(csilk_thread_create(&tid, admin_test_srv_thread, &srv) == 0);
    while (!atomic_load(&srv.ready)) {
        csilk_thread_yield();
    }

    int sock = admin_connect_localhost(port);
    if (sock < 0) {
        csilk_server_stop(csilk_app_server(app));
        csilk_thread_join(&tid);
        csilk_app_free(app);
        FAIL("could not connect to admin test server");
        return;
    }

    const char* req = "GET /admin/profile HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t     sent = send(sock, req, strlen(req), 0);
    assert(sent == (ssize_t)strlen(req));

    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char   buf[4096];
    size_t total = 0;
    buf[0] = '\0';
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") && (strstr(buf, "</svg>") || strstr(buf, "No samples"))) {
            break;
        }
    }
    buf[total] = '\0';
    close(sock);

    csilk_server_stop(csilk_app_server(app));
    csilk_thread_join(&tid);
    csilk_app_free(app);

    if (total > 0 && (strstr(buf, "HTTP/1.1 200") != nullptr ||
                      strstr(buf, "No samples collected") != nullptr)) {
        PASS();
    } else {
        FAIL("admin /profile async response missing or timed out");
    }
}

static void
test_admin_profile_disconnect_before_complete_no_uaf(void)
{
    int port = admin_pick_ephemeral_port();
    assert(port > 0);

    csilk_app_t*          app = csilk_app_new(nullptr);
    csilk_config_t*       full_cfg = csilk_app_config(app);
    csilk_server_config_t cfg = full_cfg->server;
    free(full_cfg);
    cfg.worker_threads = 1;
    /* Short request timeout forces on_close() BEFORE 150ms sampler finishes */
    cfg.read_timeout_ms = 30;
    cfg.request_timeout_ms = 30;
    csilk_app_set_server_config(app, cfg);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    admin_test_srv_t srv = {app, port, 0};
    csilk_thread_t   tid;
    assert(csilk_thread_create(&tid, admin_test_srv_thread, &srv) == 0);
    while (!atomic_load(&srv.ready)) {
        csilk_thread_yield();
    }

    int sock = admin_connect_localhost(port);
    assert(sock >= 0);

    const char* req = "GET /admin/profile HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t     sent = send(sock, req, strlen(req), 0);
    assert(sent == (ssize_t)strlen(req));

    /* Close client socket immediately and wait for timeout + sampler completion */
    close(sock);
    usleep(220000);

    csilk_server_stop(csilk_app_server(app));
    csilk_thread_join(&tid);
    csilk_app_free(app);
    PASS();
}

static void
test_admin_flamegraph_server_stop_cancels_sampler(void)
{
    int port = admin_pick_ephemeral_port();
    assert(port > 0);

    csilk_app_t*          app = csilk_app_new(nullptr);
    csilk_config_t*       full_cfg = csilk_app_config(app);
    csilk_server_config_t cfg = full_cfg->server;
    free(full_cfg);
    cfg.worker_threads = 1;
    csilk_app_set_server_config(app, cfg);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    admin_test_srv_t srv = {app, port, 0};
    csilk_thread_t   tid;
    assert(csilk_thread_create(&tid, admin_test_srv_thread, &srv) == 0);
    while (!atomic_load(&srv.ready)) {
        csilk_thread_yield();
    }

    int sock = admin_connect_localhost(port);
    assert(sock >= 0);

    /* /admin/flamegraph starts a 5-second profiler; stopping the server mid-flight
     * must cancel and join the sampler cleanly without waiting 5s or triggering UAF. */
    const char* req =
        "GET /admin/flamegraph HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t sent = send(sock, req, strlen(req), 0);
    assert(sent == (ssize_t)strlen(req));
    usleep(40000);
    close(sock);

    csilk_server_stop(csilk_app_server(app));
    csilk_thread_join(&tid);
    csilk_app_free(app);
    assert(csilk_flamegraph_is_running() == 0);
    PASS();
}

int
main(void)
{
    printf("=== Admin Module Tests ===\n\n");

    test_admin_enable_metrics_collection();
    test_admin_stats_collection();
    test_admin_ctx_storage_overflow();
    test_admin_serve_requires_auth();
    test_admin_serve_secure_and_stats_endpoint();
    test_admin_stats_reflects_optional_subsystems();
    test_admin_profile_async_response();
    test_admin_profile_disconnect_before_complete_no_uaf();
    test_admin_flamegraph_server_stop_cancels_sampler();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
