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

/*
 * P3 regression test for csilk#9d: JSON-RPC 2.0 response frames.
 *
 * csilk_respond_jsonrpc_result / csilk_respond_jsonrpc_error emit
 * well-formed JSON-RPC frames so a JSON-RPC endpoint does not have to
 * hand-roll the {result, id} / {error: {code, message, data}, id} shape.
 *
 * The test spins up a minimal app server with a /rpc.json route that
 * returns a success frame and an error frame, then asserts the body
 * shape over a raw socket.
 */

#define PORT 18691
#define BUFSIZE 65536

static _Atomic int     server_ready = 0;
static csilk_server_t* g_server = nullptr;
static csilk_app_t*    g_app = nullptr;
static int             g_tests_passed = 0;
static int             g_tests_failed = 0;

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
        char* hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            char* cl = strstr(buf, "Content-Length: ");
            if (!cl) {
                cl = strstr(buf, "content-length: ");
            }
            if (cl) {
                long clen = strtol(cl + 16, nullptr, 10);
                if (total >= (int)(hdr_end - buf + 4 + clen)) {
                    break;
                }
            }
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
    for (int retry = 0; retry < 50; retry++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return -1;
        }
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(PORT);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return sock;
        }
        close(sock);
        usleep(10000);
    }
    return -1;
}

static int
send_request(int sock, const char* req)
{
    return (int)send(sock, req, strlen(req), 0);
}

/* ---- Handler: emit JSON-RPC success / error frames ---- */
static void
rpc_json_handler(csilk_ctx_t* c)
{
    const char*   id = csilk_get_query(c, "id");
    csilk_json_t* id_json = id ? csilk_json_string_new(id) : NULL;

    const char* method = csilk_get_query(c, "method");
    if (method && strcmp(method, "echo") == 0) {
        /* Success frame: result = {ok:true}, id echoed. */
        csilk_json_t* result = csilk_json_object();
        csilk_json_add_int(result, "ok", 1);
        csilk_respond_jsonrpc_result(c, result, id_json);
        return;
    }
    if (method && strcmp(method, "unknown") == 0) {
        /* Error frame: code -32601 (Method not found), HTTP 200. */
        csilk_respond_jsonrpc_error(c, -32601, "Method not found", NULL, id_json, 200);
        return;
    }
    /* Default: success with null result and no id. */
    csilk_respond_jsonrpc_result(c, NULL, NULL);
}

/* ---- Server thread ---- */
static void*
run_app(void* arg)
{
    (void)arg;
    g_app = csilk_app_new(nullptr);
    csilk_app_get(g_app, "/rpc.json", rpc_json_handler);
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
test_success_frame(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("success frame (connect)", 0);
        return;
    }
    const char* req = "GET /rpc.json?method=echo&id=42 HTTP/1.1\r\n"
                      "Host: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("success frame (response received)", n > 0);
    test_result("success frame (status 200)", expect_status(buf, 200));
    test_result("success frame (has result)", expect_body(buf, "\"result\""));
    test_result("success frame (has id)", expect_body(buf, "\"id\""));
    test_result("success frame (ok:true)", expect_body(buf, "\"ok\":1"));
}

static void
test_error_frame(void)
{
    int sock = connect_server();
    if (sock < 0) {
        test_result("error frame (connect)", 0);
        return;
    }
    const char* req = "GET /rpc.json?method=unknown&id=42 HTTP/1.1\r\n"
                      "Host: localhost\r\nConnection: close\r\n\r\n";
    send_request(sock, req);
    char buf[BUFSIZE] = {0};
    int  n = read_response(sock, buf, sizeof(buf));
    close(sock);
    test_result("error frame (response received)", n > 0);
    test_result("error frame (has error)", expect_body(buf, "\"error\""));
    test_result("error frame (code -32601)", expect_body(buf, "-32601"));
    test_result("error frame (message)", expect_body(buf, "Method not found"));
}

int
main(void)
{
    pthread_t th;
    pthread_create(&th, NULL, run_app, NULL);

    for (int i = 0; i < 100 && !server_ready; i++) {
        usleep(50000);
    }

    test_success_frame();
    test_error_frame();

    printf("test_jsonrpc_frames: %d passed, %d failed\n", g_tests_passed, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
