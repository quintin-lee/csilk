#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define PORT 8096
#define BUFSIZE 4096

static volatile int    server_ready = 0;
static csilk_server_t* g_server = nullptr;

static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, CSILK_STATUS_OK, "Hello");
}

static void
echo_handler(csilk_ctx_t* c)
{
    const char* name = csilk_get_query(c, "name");
    char        resp[256];
    snprintf(resp, sizeof(resp), "Echo: %s", name ? name : "none");
    csilk_string(c, CSILK_STATUS_OK, resp);
}

static void
on_server_start(csilk_server_t* s)
{
    (void)s;
    server_ready = 1;
}

static void*
run_server(void* arg)
{
    (void)arg;
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h1[] = {hello_handler};
    csilk_router_add(router, "GET", "/", h1, 1);
    csilk_handler_t h2[] = {echo_handler};
    csilk_router_add(router, "GET", "/echo", h2, 1);

    g_server = csilk_server_new(router);
    csilk_server_add_hook(g_server, CSILK_HOOK_SERVER_START, on_server_start);
    csilk_server_run(g_server, PORT);
    csilk_server_free(g_server);
    csilk_router_free(router);
    return nullptr;
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
send_req(int sock, const char* req)
{
    return send(sock, req, strlen(req), 0) > 0 ? 0 : -1;
}

static int
recv_resp(int sock, char* buf, size_t size)
{
    fd_set         fds;
    struct timeval tv = {3, 0};
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
    if (ret <= 0) {
        return -1;
    }
    int n = recv(sock, buf, size - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
    }
    return n;
}

static int
expect_status(const char* resp, int expected)
{
    char exp[32];
    snprintf(exp, sizeof(exp), "HTTP/1.1 %d", expected);
    return strstr(resp, exp) != nullptr;
}

static int
expect_body(const char* resp, const char* body)
{
    const char* hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) {
        return 0;
    }
    return strstr(hdr_end + 4, body) != nullptr;
}

int
main()
{
    pthread_t thread;
    pthread_create(&thread, nullptr, run_server, nullptr);
    while (!server_ready) {
    }
    usleep(100000);

    int         passed = 0, failed = 0;
    const char* req1 = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: "
                       "keep-alive\r\n\r\n";
    const char* req2 = "GET /echo?name=keepalive HTTP/1.1\r\nHost: "
                       "localhost\r\nConnection: "
                       "close\r\n\r\n";

    printf("Keep-alive test: connecting...\n");

    int sock = connect_server();
    if (sock < 0) {
        printf("FAIL: connect\n");
        csilk_server_stop(g_server);
        pthread_join(thread, nullptr);
        return 1;
    }

    // Request 1
    if (send_req(sock, req1) < 0) {
        printf("FAIL: send req1\n");
        close(sock);
        csilk_server_stop(g_server);
        pthread_join(thread, nullptr);
        return 1;
    }

    char buf1[BUFSIZE] = {0};
    int  n1 = recv_resp(sock, buf1, sizeof(buf1));
    int  ok1 = (n1 > 0 && expect_status(buf1, 200) && expect_body(buf1, "Hello"));
    printf("  Request 1 (keep-alive): %s\n", ok1 ? "PASS" : "FAIL");
    if (!ok1) {
        printf("    Response: %s\n", buf1);
    }
    if (ok1) {
        passed++;
    } else {
        failed++;
    }

    // Request 2 on same connection
    if (send_req(sock, req2) < 0) {
        printf("FAIL: send req2\n");
        close(sock);
        csilk_server_stop(g_server);
        pthread_join(thread, nullptr);
        return 1;
    }

    char buf2[BUFSIZE] = {0};
    int  n2 = recv_resp(sock, buf2, sizeof(buf2));
    int  ok2 = (n2 > 0 && expect_status(buf2, 200) && expect_body(buf2, "Echo: keepalive"));
    printf("  Request 2 (same connection): %s\n", ok2 ? "PASS" : "FAIL");
    if (!ok2) {
        printf("    Response: %s\n", buf2);
    }
    if (ok2) {
        passed++;
    } else {
        failed++;
    }

    close(sock);
    csilk_server_stop(g_server);
    // Give the server thread a moment to process the stop async and close handles
    usleep(200000);
    pthread_join(thread, nullptr);

    printf("Keep-alive test: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
