/**
 * @file test_integration_ext.c
 * @brief Extended end-to-end integration tests.
 *
 * Starts a real csilk server on a test port and sends raw HTTP requests
 * via BSD sockets, verifying status codes and response bodies.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* csilk/app/workflow.h MUST come BEFORE csilk/csilk.h to avoid
   the duplicate CSILK_WORKFLOW_H guard in the shim csilk/workflow.h */
#include "csilk/app/workflow.h"
#include "csilk/csilk.h"
#include "csilk/app/app.h"

#define PORT 8101
#define BUFSIZE 16384

static int         g_tests_passed = 0;
static int         g_tests_failed = 0;
static csilk_wf_t* g_wf = nullptr;

static void
test_result(const char* name, int ok)
{
    if (ok) {
        printf("  PASS: %s\n", name);
        g_tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        g_tests_failed++;
    }
}

static int
recv_response(int sock, char* buf, size_t size)
{
    fd_set         fds;
    struct timeval tv = {3, 0};
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
    if (ret <= 0) {
        return -1;
    }
    return recv(sock, buf, size - 1, 0);
}

static int
expect_status(const char* resp, int expected)
{
    char expected_str[32];
    snprintf(expected_str, sizeof(expected_str), "HTTP/1.1 %d", expected);
    return strstr(resp, expected_str) != nullptr;
}

static int
expect_body(const char* resp, const char* body)
{
    const char* hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) {
        return 0;
    }
    const char* body_start = hdr_end + 4;
    return strstr(body_start, body) != nullptr;
}

static int
connect_server()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(PORT);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static int
send_request(int sock, const char* req)
{
    return send(sock, req, strlen(req), 0);
}

/* ---- Handlers ---- */

static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "Hello, Extended!");
}

static void
mq_test_publish(csilk_ctx_t* c)
{
    csilk_mq_t* mq = csilk_server_get_mq(csilk_ctx_get_server(c));
    if (mq) {
        csilk_mq_publish(mq, "test.topic", "payload", 7);
        csilk_string(c, CSILK_STATUS_OK, "MQ published");
    } else {
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "No MQ");
    }
}

static void
mq_test_subscriber(csilk_mq_ctx_t* ctx)
{
    (void)ctx;
    /* no-op subscriber for integration test */
}

static void
mq_test_middleware(csilk_mq_ctx_t* ctx)
{
    csilk_mq_next(ctx);
}

static void
wf_run_handler(csilk_ctx_t* c)
{
    csilk_data_t input = {.type = "text/plain", .value = "world"};
    csilk_wf_run(g_wf, &input, nullptr);
    csilk_string(c, CSILK_STATUS_OK, "WF started");
}

static csilk_data_t*
wf_greeting(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)user_data;
    char* result = csilk_wf_strdup(ctx, "Hello");
    return csilk_wf_data_new(ctx, "text/plain", result);
}

static void
admin_stats_check(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "Admin OK");
}

/* ---- Server thread ---- */

static volatile int server_ready = 0;

static void*
run_server(void* arg)
{
    (void)arg;
    csilk_app_t* app = csilk_app_new(nullptr);

    /* Basic routes */
    csilk_app_get(app, "/", hello_handler);

    /* --- MQ routes --- */
    csilk_mq_t* mq = csilk_server_get_mq(csilk_app_server(app));
    csilk_mq_subscribe(mq, "test.topic", mq_test_subscriber);
    csilk_mq_use(mq, nullptr, mq_test_middleware);
    csilk_app_get(app, "/mq/publish", mq_test_publish);

    /* --- Admin Dashboard --- */
    csilk_admin_serve(app, "/admin");
    csilk_app_get(app, "/admin/status", admin_stats_check);

    /* --- Workflow --- */
    g_wf = csilk_wf_new("integration_test");
    csilk_wf_node_t* n1 = csilk_wf_add(g_wf, "greet", wf_greeting, nullptr);
    csilk_wf_node_set_entry(n1, 1);
    csilk_wf_run(g_wf, nullptr, nullptr);
    csilk_app_get(app, "/wf/run", wf_run_handler);

    /* CORS middleware omitted — requires parameterized setup via context storage */

    server_ready = 1;
    csilk_app_run(app, PORT);
    csilk_app_free(app);
    return nullptr;
}

/* ---- Tests ---- */

static void
test_get_root()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET / (connect)", 0);
        return;
    }
    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: GET / (response)", n > 0);
    test_result("ext: GET / (status 200)", expect_status(buf, CSILK_STATUS_OK));
    test_result("ext: GET / (body)", expect_body(buf, "Hello, Extended!"));
}

static void
test_mq_publish()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("MQ publish (connect)", 0);
        return;
    }
    const char* req = "GET /mq/publish HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: MQ publish (response)", n > 0);
    test_result("ext: MQ publish (status 200)", expect_status(buf, CSILK_STATUS_OK));
    test_result("ext: MQ publish (body)", expect_body(buf, "MQ published"));
}

static void
test_admin_ui()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("Admin UI (connect)", 0);
        return;
    }
    const char* req = "GET /admin/ HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: Admin UI (response)", n > 0);
    /* Admin UI serves HTML — status 200 */
    test_result("ext: Admin UI (status 200)", expect_status(buf, CSILK_STATUS_OK));
}

static void
test_admin_stats()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("Admin stats (connect)", 0);
        return;
    }
    const char* req = "GET /admin/stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: Admin stats (response)", n > 0);
    test_result("ext: Admin stats (status 200)", expect_status(buf, CSILK_STATUS_OK));
    /* Verify JSON structure */
    test_result("ext: Admin stats (JSON)", expect_body(buf, "total_requests"));
}

static void
test_workflow_run()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("WF run (connect)", 0);
        return;
    }
    const char* req = "GET /wf/run HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: WF run (response)", n > 0);
    test_result("ext: WF run (status 200)", expect_status(buf, CSILK_STATUS_OK));
    test_result("ext: WF run (body)", expect_body(buf, "WF started"));
}

static void
test_keepalive_multi()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("keepalive multi (connect)", 0);
        return;
    }

    const char* req1 = "GET / HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: keep-alive\r\n\r\n";
    const char* req2 = "GET /mq/publish HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: keep-alive\r\n\r\n";
    const char* req3 = "GET /admin/stats HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: close\r\n\r\n";

    send_request(sock, req1);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    int  ok1 = n > 0 && expect_status(buf, CSILK_STATUS_OK) && expect_body(buf, "Hello, Extended!");

    memset(buf, 0, sizeof(buf));
    send_request(sock, req2);
    n = recv_response(sock, buf, sizeof(buf));
    int ok2 = n > 0 && expect_status(buf, CSILK_STATUS_OK) && expect_body(buf, "MQ published");

    memset(buf, 0, sizeof(buf));
    send_request(sock, req3);
    n = recv_response(sock, buf, sizeof(buf));
    int ok3 = n > 0 && expect_status(buf, CSILK_STATUS_OK) && expect_body(buf, "total_requests");

    close(sock);
    test_result("ext: keepalive req1: GET /", ok1);
    test_result("ext: keepalive req2: GET /mq/publish", ok2);
    test_result("ext: keepalive req3: GET /admin/stats", ok3);
}

static void
test_404_not_found()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("404 (connect)", 0);
        return;
    }
    const char* req = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: 404 (response)", n > 0);
    test_result("ext: 404 (status 404)", expect_status(buf, CSILK_STATUS_NOT_FOUND));
}

static void
test_get_with_query()
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET with query (connect)", 0);
        return;
    }
    const char* req = "GET /?foo=bar HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = recv_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("ext: GET with query (response)", n > 0);
    test_result("ext: GET with query (status 200)", expect_status(buf, CSILK_STATUS_OK));
}

int
main()
{
    printf("=== Extended Integration Tests ===\n\n");

    pthread_t thread;
    pthread_create(&thread, nullptr, run_server, nullptr);
    while (!server_ready) {
        nanosleep(&(struct timespec){0, 10000000}, nullptr);
    }
    nanosleep(&(struct timespec){0, 50000000}, nullptr);

    test_get_root();
    test_mq_publish();
    test_admin_ui();
    test_admin_stats();
    test_workflow_run();
    test_keepalive_multi();
    test_404_not_found();
    test_get_with_query();

    printf("\n=== Extended Integration Results: %d passed, %d failed ===\n",
           g_tests_passed,
           g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
