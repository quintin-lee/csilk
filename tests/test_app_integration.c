#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

#include "csilk/app/app.h"
#include "csilk/csilk.h"

#define PORT 8101
#define BUFSIZE 65536

static volatile int    server_ready = 0;
static csilk_server_t* g_server = nullptr;
static csilk_app_t*    g_app = nullptr;

static int g_tests_passed = 0;
static int g_tests_failed = 0;

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
read_response(int sock, char* buf, size_t size)
{
    fd_set         fds;
    struct timeval tv = {3, 0};
    int            total = 0;
    while (total < (int)size - 1) {
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) {
            break;
        }
        int n = (int)recv(sock, buf + total, (int)size - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") && total > (int)(strstr(buf, "\r\n\r\n") - buf + 4)) {
            break;
        }
    }
    return total;
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
connect_server(void)
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
    return (int)send(sock, req, strlen(req), 0);
}

/* ---- Custom handler ---- */
static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "App Integration Hello!");
}

/* ---- Server thread ---- */
static void*
run_app(void* arg)
{
    (void)arg;
    g_app = csilk_app_new(nullptr);
    csilk_app_log_json(g_app, 1);
    csilk_app_get(g_app, "/hello", hello_handler);
    g_server = csilk_app_server(g_app);
    atomic_thread_fence(memory_order_seq_cst);
    server_ready = 1;
    csilk_app_run(g_app, PORT);
    csilk_app_free(g_app);
    g_app = nullptr;
    return nullptr;
}

/* ---- Tests ---- */
static void
test_get_hello(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /hello (connect)", 0);
        return;
    }
    const char* req = "GET /hello HTTP/1.1\r\nHost: "
                      "localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("GET /hello (response received)", n > 0);
    test_result("GET /hello (status 200)", expect_status(buf, 200));
    test_result("GET /hello (body)", expect_body(buf, "App Integration Hello!"));
}

static void
test_openapi_json(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /openapi.json (connect)", 0);
        return;
    }
    const char* req = "GET /openapi.json HTTP/1.1\r\nHost: localhost\r\nConnection: "
                      "close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("GET /openapi.json (response received)", n > 0);
    test_result("GET /openapi.json (status 200)", expect_status(buf, 200));
    test_result("GET /openapi.json (JSON body)", expect_body(buf, "\"openapi\""));
    test_result("GET /openapi.json (paths)", expect_body(buf, "\"/hello\""));
}

static void
test_docs(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /docs (connect)", 0);
        return;
    }
    const char* req = "GET /docs HTTP/1.1\r\nHost: "
                      "localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("GET /docs (response received)", n > 0);
    test_result("GET /docs (status 200)", expect_status(buf, 200));
    test_result("GET /docs (HTML body)", expect_body(buf, "<html"));
}

static void
test_csilk_docs_static(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /csilk-docs/index.css (connect)", 0);
        return;
    }
    const char* req = "GET /csilk-docs/index.css HTTP/1.1\r\nHost: "
                      "localhost\r\nConnection: "
                      "close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("GET /csilk-docs/index.css (response received)", n > 0);
    test_result("GET /csilk-docs/index.css (status 200)", expect_status(buf, 200));
    int has_html = expect_body(buf, "html");
    if (!has_html) {
        const char* hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            printf("FAIL CSS: body=\n%s\n...\n", hdr_end + 4);
        }
    }
    test_result("GET /csilk-docs/index.css (CSS content)", has_html);
}

static void
test_openapi_disabled(void)
{
    /* Disable OpenAPI to trigger the 404 fallback path */
    csilk_app_enable_openapi(g_app, 0);
    usleep(50000);

    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /openapi.json disabled (connect)", 0);
        csilk_app_enable_openapi(g_app, 1);
        return;
    }
    const char* req = "GET /openapi.json HTTP/1.1\r\nHost: localhost\r\nConnection: "
                      "close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);

    csilk_app_enable_openapi(g_app, 1);
    usleep(50000);

    test_result("GET /openapi.json disabled (response received)", n > 0);
    test_result("GET /openapi.json disabled (status 404)", expect_status(buf, 404));
}

static void
test_not_found(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("GET /nonexistent (connect)", 0);
        return;
    }
    const char* req = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: "
                      "close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("GET /nonexistent (response received)", n > 0);
    test_result("GET /nonexistent (status 404)", expect_status(buf, 404));
}

int
main(void)
{
    printf("=== App Integration Tests ===\n\n");

    pthread_t thread;
    pthread_create(&thread, nullptr, run_app, nullptr);
    while (!server_ready) {
        nanosleep(&(struct timespec){0, 10000000}, nullptr);
    }
    nanosleep(&(struct timespec){0, 100000000}, nullptr);

    test_get_hello();
    test_openapi_json();
    test_docs();
    test_csilk_docs_static();
    test_openapi_disabled();
    test_not_found();

    csilk_server_stop(g_server);
    pthread_join(thread, nullptr);

    printf("\n=== Results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);
    return g_tests_failed > 0 ? 1 : 0;
}
