#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "csilk/app/app.h"
#include "csilk/csilk.h"
#include "csilk/protocols/sse.h"
#define PORT 8215
#define BUF 8192
static int             pass = 0, fail = 0;
static csilk_server_t* srv = NULL;
#define TR(n, o)                                                                                   \
    do {                                                                                           \
        if (o) {                                                                                   \
            printf("  PASS: %s\n", n);                                                             \
            pass++;                                                                                \
        } else {                                                                                   \
            printf("  FAIL: %s\n", n);                                                             \
            fail++;                                                                                \
        }                                                                                          \
    } while (0)
static int
conn(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1"), .sin_port = htons(PORT)};
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}
static int
recv_resp(int s, char* b, size_t sz)
{
    fd_set f;
    int    t = 0;
    while (t < (int)sz - 1) {
        struct timeval tv = {10, 0};
        FD_ZERO(&f);
        FD_SET(s, &f);
        if (select(s + 1, &f, NULL, NULL, &tv) <= 0) {
            break;
        }
        int n = recv(s, b + t, (int)sz - 1 - t, 0);
        if (n <= 0) {
            break;
        }
        t += n;
        b[t] = 0;
        if (strstr(b, "\r\n\r\n")) {
            char* cl = strstr(b, "Content-Length: ");
            if (!cl) {
                cl = strstr(b, "content-length: ");
            }
            if (cl) {
                int cln = atoi(cl + 16);
                int bl = t - (strstr(b, "\r\n\r\n") + 4 - b);
                if (bl >= cln) {
                    break;
                }
            } else {
                break;
            }
        }
    }
    return t;
}
static int
exp_status(const char* r, int e)
{
    char s[32];
    snprintf(s, 32, "HTTP/1.1 %d", e);
    return strstr(r, s) != NULL;
}
static int
exp_body(const char* r, const char* b)
{
    const char* p = strstr(r, "\r\n\r\n");
    return p ? (strstr(p + 4, b) != NULL) : 0;
}
static void
sse_h(csilk_ctx_t* c)
{
    csilk_sse_init(c);
    csilk_sse_send(c, "message", "hello world");
    csilk_sse_close(c);
}
static _Atomic int ready = 0;
static void*
run_srv(void* a)
{
    (void)a;
    csilk_app_t* app = csilk_app_new(NULL);
    csilk_app_get(app, "/events", sse_h);
    ready = 1;
    srv = csilk_app_server(app);
    csilk_app_run(app, PORT);
    csilk_app_free(app);
    return NULL;
}
static void
t_sse_connect(void)
{
    int s = conn();
    if (s < 0) {
        TR("sse connect", 0);
        return;
    }
    const char* r = "GET /events HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(s, r, strlen(r), 0);
    usleep(50000);
    char b[BUF] = {0};
    int  n = recv(s, b, sizeof(b) - 1, 0);
    close(s);
    TR("sse resp", n > 0);
    TR("sse 200", exp_status(b, 200));
    TR("sse content-type", strstr(b, "text/event-stream") != NULL);
    TR("sse body", strstr(b, "hello world") != NULL);
}
static void
t_sse_headers(void)
{
    int s = conn();
    if (s < 0) {
        TR("headers connect", 0);
        return;
    }
    const char* r = "GET /events HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(s, r, strlen(r), 0);
    usleep(50000);
    char b[BUF] = {0};
    int  n = recv(s, b, sizeof(b) - 1, 0);
    close(s);
    TR("headers resp", n > 0);
    TR("headers cache-control", strstr(b, "no-cache") != NULL);
    TR("headers x-accel", strstr(b, "X-Accel-Buffering: no") != NULL);
}
int
main(void)
{
    printf("=== SSE Integration ===\n\n");
    signal(SIGPIPE, SIG_IGN);
    pthread_t th;
    pthread_create(&th, NULL, run_srv, NULL);
    while (!ready) {
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    }
    nanosleep(&(struct timespec){0, 50000000}, NULL);
    t_sse_connect();
    t_sse_headers();
    if (srv) {
        csilk_server_stop(srv);
    }
    pthread_join(th, NULL);
    printf("\n=== %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
