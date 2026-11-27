/**
 * @file test_io_equivalence.c
 * @brief Behavioral equivalence suite shared across libuv and io_uring backends.
 *
 * Verifies identical observable behavior across both backends:
 *   1. Loop & monotonic clock + one-shot / repeating / restarted timers
 *   2. Cross-thread csilk_io_async_send wakeups & close callbacks with .data
 *   3. Raw TCP loopback bind/listen/accept/read_start/write/close lifecycle
 *   4. Multi-worker csilk_server_t HTTP/1.1 parity (GET, POST, 404, streaming,
 *      csilk_dispatch async completion, and keep-alive reuse)
 *   5. Optional `--emit-json <path>` output for automated cross-backend diffing.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/core/server/hot_reload.h"
#include "csilk/core/sync.h"
#include "csilk/core/sys_io.h"
#include "csilk/csilk.h"

typedef struct {
    const char* backend;
    int         timer_oneshot_count;
    int         timer_repeat_count;
    int         timer_close_data_ok;
    int         async_wakeups;
    int         async_close_data_ok;
    int         tcp_echo_bytes;
    int         tcp_close_cbs;
    int         http_get_status;
    char        http_get_body[64];
    int         http_post_status;
    char        http_post_body[64];
    int         http_404_status;
    char        http_stream_body[64];
    int         http_dispatch_status;
    char        http_dispatch_body[64];
    int         keepalive_ok;
} eq_report_t;

static eq_report_t g_rep = {0};

/* ====================================================================
 * Helpers: ephemeral port & HTTP/1.1 client
 * ==================================================================== */

static int
reserve_ephemeral_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int
tcp_connect_loopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    int rc;
    do {
        rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int
tcp_connect_loopback_retry(int port, int max_attempts)
{
    int last_errno = 0;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int fd = tcp_connect_loopback(port);
        if (fd >= 0) {
            return fd;
        }
        last_errno = errno;
        if (last_errno != ECONNREFUSED && last_errno != EAGAIN && last_errno != EINTR &&
            last_errno != EADDRNOTAVAIL) {
            break;
        }
        csilk_io_sleep(10);
    }
    fprintf(stderr,
            "tcp_connect_loopback_retry(port=%d) failed after %d attempt(s): %s (errno=%d)\n",
            port,
            max_attempts,
            strerror(last_errno),
            last_errno);
    return -1;
}

static int
send_all(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int
read_http_response(int fd, int* out_status, char* out_body, size_t body_cap)
{
    char   buf[8192];
    size_t used = 0;
    int    status = 0;
    long   content_len = -1;
    int    is_chunked = 0;
    char*  hdr_end = NULL;

    while (used < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(
                stderr, "read_http_response recv error: %s (used=%zu)\n", strerror(errno), used);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
        buf[used] = '\0';

        if (!hdr_end) {
            hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                sscanf(buf, "HTTP/1.%*d %d", &status);
                char* cl = strstr(buf, "Content-Length:");
                if (!cl) {
                    cl = strstr(buf, "content-length:");
                }
                if (cl && cl < hdr_end) {
                    content_len = strtol(cl + 15, NULL, 10);
                }
                if (strstr(buf, "Transfer-Encoding: chunked") &&
                    strstr(buf, "Transfer-Encoding: chunked") < hdr_end) {
                    is_chunked = 1;
                }
            }
        }

        if (hdr_end) {
            size_t hdr_bytes = (size_t)(hdr_end + 4 - buf);
            size_t body_have = used - hdr_bytes;
            if (is_chunked) {
                if (strstr(hdr_end + 4, "0\r\n\r\n") != NULL) {
                    break;
                }
            } else if (content_len >= 0 && body_have >= (size_t)content_len) {
                break;
            }
        }
    }

    if (!hdr_end || status == 0) {
        return -1;
    }
    *out_status = status;

    const char* raw_body = hdr_end + 4;
    if (is_chunked) {
        size_t pos = 0;
        out_body[0] = '\0';
        const char* p = raw_body;
        while (*p) {
            char* endptr = NULL;
            long  chunk_sz = strtol(p, &endptr, 16);
            if (!endptr || endptr[0] != '\r' || endptr[1] != '\n') {
                break;
            }
            p = endptr + 2;
            if (chunk_sz <= 0) {
                break;
            }
            size_t copy_n = (size_t)chunk_sz;
            if (pos + copy_n >= body_cap) {
                copy_n = body_cap - 1 - pos;
            }
            memcpy(out_body + pos, p, copy_n);
            pos += copy_n;
            out_body[pos] = '\0';
            p += (size_t)chunk_sz + 2;
        }
    } else if (content_len > 0) {
        size_t copy_n = (size_t)content_len < body_cap - 1 ? (size_t)content_len : body_cap - 1;
        memcpy(out_body, raw_body, copy_n);
        out_body[copy_n] = '\0';
    } else {
        out_body[0] = '\0';
    }
    return 0;
}

/* ====================================================================
 * Test 1: Timer & Close .data Parity
 * ==================================================================== */

static int g_eq_sentinel = 0x42;

typedef struct {
    int oneshot_hits;
    int repeat_hits;
    int close_ok;
} timer_eq_state_t;

static void
on_oneshot_timer(csilk_io_timer_t* h)
{
    timer_eq_state_t* st = (timer_eq_state_t*)h->data;
    st->oneshot_hits++;
}

static void
on_repeat_timer(csilk_io_timer_t* h)
{
    timer_eq_state_t* st = (timer_eq_state_t*)h->data;
    st->repeat_hits++;
    if (st->repeat_hits >= 3) {
        csilk_io_timer_stop(h);
        csilk_io_stop(h->loop);
    }
}

static void
on_timer_close_cb(csilk_io_handle_t* h)
{
    timer_eq_state_t* st = (timer_eq_state_t*)h->data;
    if (st != NULL) {
        st->close_ok++;
    }
}

static void
test_timer_equivalence(void)
{
    csilk_io_loop_t loop;
    assert(csilk_io_loop_init(&loop) == 0);
    uint64_t t0 = csilk_io_now(&loop);
    assert(t0 > 0);
    assert(csilk_io_hrtime() > 0);

    timer_eq_state_t st = {0};
    csilk_io_timer_t t1, t2;
    assert(csilk_io_timer_init(&loop, &t1) == 0);
    assert(csilk_io_timer_init(&loop, &t2) == 0);
    t1.data = &st;
    t2.data = &st;

    assert(csilk_io_timer_start(&t1, on_oneshot_timer, 5, 0) == 0);
    assert(csilk_io_timer_start(&t2, on_repeat_timer, 5, 5) == 0);
    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);

    assert(st.oneshot_hits == 1);
    assert(st.repeat_hits == 3);

    csilk_io_close((csilk_io_handle_t*)&t1, on_timer_close_cb);
    csilk_io_close((csilk_io_handle_t*)&t2, on_timer_close_cb);
    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    assert(st.close_ok == 2);
    assert(csilk_io_loop_close(&loop) == 0);

    g_rep.timer_oneshot_count = st.oneshot_hits;
    g_rep.timer_repeat_count = st.repeat_hits;
    g_rep.timer_close_data_ok = st.close_ok;
}

/* ====================================================================
 * Test 2: Async Wakeup Parity
 * ==================================================================== */

typedef struct {
    csilk_io_async_t* async;
    int               target;
} async_sender_ctx_t;

static atomic_int g_async_hits = 0;
static int        g_async_close_ok = 0;

static void
on_eq_async(csilk_io_async_t* h)
{
    assert(h->data == &g_eq_sentinel);
    int c = atomic_fetch_add(&g_async_hits, 1) + 1;
    if (c >= 1) {
        csilk_io_stop(h->loop);
    }
}

static void
on_eq_async_close(csilk_io_handle_t* h)
{
    if (h->data == &g_eq_sentinel) {
        g_async_close_ok = 1;
    }
}

static void
async_sender_fn(void* arg)
{
    async_sender_ctx_t* actx = (async_sender_ctx_t*)arg;
    csilk_io_sleep(10);
    for (int i = 0; i < actx->target; i++) {
        assert(csilk_io_async_send(actx->async) == 0);
    }
}

static void
test_async_equivalence(void)
{
    csilk_io_loop_t loop;
    assert(csilk_io_loop_init(&loop) == 0);

    csilk_io_async_t async;
    assert(csilk_io_async_init(&loop, &async, on_eq_async) == 0);
    async.data = &g_eq_sentinel;

    atomic_store(&g_async_hits, 0);
    g_async_close_ok = 0;

    async_sender_ctx_t actx = {.async = &async, .target = 3};
    csilk_thread_t     th;
    assert(csilk_thread_create(&th, async_sender_fn, &actx) == 0);

    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    assert(csilk_thread_join(&th) == 0);
    assert(atomic_load(&g_async_hits) >= 1);

    csilk_io_close((csilk_io_handle_t*)&async, on_eq_async_close);
    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    assert(g_async_close_ok == 1);
    assert(csilk_io_loop_close(&loop) == 0);

    g_rep.async_wakeups = 1; /* coalesced >= 1 deterministic boolean */
    g_rep.async_close_data_ok = g_async_close_ok;
}

/* ====================================================================
 * Test 3: Raw TCP Loopback Scatter Write & EOF Parity
 * ==================================================================== */

static csilk_io_tcp_t   g_tcp_server;
static csilk_io_tcp_t   g_tcp_peer;
static csilk_io_write_t g_tcp_wreq;
static char             g_tcp_read_buf[1024];
static char             g_tcp_rx_accum[256];
static size_t           g_tcp_rx_len = 0;
static int              g_tcp_close_count = 0;

static void
eq_alloc_cb(csilk_io_handle_t* h, size_t suggested, csilk_io_buf_t* buf)
{
    (void)h;
    (void)suggested;
    *buf = csilk_io_buf_init(g_tcp_read_buf, (unsigned int)sizeof(g_tcp_read_buf));
}

static void
on_tcp_handle_closed(csilk_io_handle_t* h)
{
    (void)h;
    g_tcp_close_count++;
}

static void
on_tcp_write_done(csilk_io_write_t* req, int status)
{
    (void)req;
    assert(status >= 0);
}

static void
on_tcp_read(csilk_io_stream_t* stream, ssize_t nread, const csilk_io_buf_t* buf)
{
    (void)buf;
    if (nread > 0) {
        size_t copy_n = (size_t)nread;
        if (g_tcp_rx_len + copy_n < sizeof(g_tcp_rx_accum)) {
            memcpy(g_tcp_rx_accum + g_tcp_rx_len, buf->base, copy_n);
            g_tcp_rx_len += copy_n;
            g_tcp_rx_accum[g_tcp_rx_len] = '\0';
        }
        csilk_io_buf_t out[2];
        out[0] = csilk_io_buf_init("ECHO:", 5);
        out[1] = csilk_io_buf_init(g_tcp_rx_accum, (unsigned int)g_tcp_rx_len);
        assert(csilk_io_write(&g_tcp_wreq, stream, out, 2, on_tcp_write_done) == 0);
    } else if (nread < 0) {
        csilk_io_read_stop(stream);
        csilk_io_close((csilk_io_handle_t*)stream, on_tcp_handle_closed);
        csilk_io_close((csilk_io_handle_t*)&g_tcp_server, on_tcp_handle_closed);
    }
}

static void
on_tcp_connection(csilk_io_stream_t* server, int status)
{
    assert(status == 0);
    assert(csilk_io_tcp_init(server->loop, &g_tcp_peer) == 0);
    g_tcp_peer.data = NULL; /* Must be NULL when using csilk_io_write directly */
    assert(csilk_io_accept(server, (csilk_io_stream_t*)&g_tcp_peer) == 0);
    assert(csilk_io_read_start((csilk_io_stream_t*)&g_tcp_peer, eq_alloc_cb, on_tcp_read) == 0);
}

static void
tcp_client_thread_fn(void* arg)
{
    int port = *(int*)arg;
    csilk_io_sleep(10);
    int fd = tcp_connect_loopback_retry(port, 20);
    assert(fd >= 0);
    assert(send_all(fd, "hello-io", 8) == 0);
    char    reply[64] = {0};
    ssize_t total = 0;
    while (total < 13) {
        ssize_t n = recv(fd, reply + total, sizeof(reply) - 1 - (size_t)total, 0);
        assert(n > 0);
        total += n;
    }
    reply[total] = '\0';
    assert(strcmp(reply, "ECHO:hello-io") == 0);
    shutdown(fd, SHUT_WR);
    close(fd);
}

static void
test_tcp_loopback_equivalence(void)
{
    csilk_io_loop_t loop;
    assert(csilk_io_loop_init(&loop) == 0);

    g_tcp_rx_len = 0;
    g_tcp_close_count = 0;

    assert(csilk_io_tcp_init(&loop, &g_tcp_server) == 0);
    struct sockaddr_in bind_addr;
    assert(csilk_io_ip4_addr("127.0.0.1", 0, &bind_addr) == 0);
    assert(csilk_io_tcp_bind(&g_tcp_server, (const struct sockaddr*)&bind_addr, 0) == 0);

    struct sockaddr_in bound_addr;
    int                blen = sizeof(bound_addr);
    assert(csilk_io_tcp_getsockname(&g_tcp_server, (struct sockaddr*)&bound_addr, &blen) == 0);
    int port = ntohs(bound_addr.sin_port);
    assert(port > 0);

    assert(csilk_io_listen((csilk_io_stream_t*)&g_tcp_server, 16, on_tcp_connection) == 0);

    csilk_thread_t th;
    assert(csilk_thread_create(&th, tcp_client_thread_fn, &port) == 0);

    csilk_io_run(&loop, CSILK_IO_RUN_DEFAULT);
    assert(csilk_thread_join(&th) == 0);
    assert(g_tcp_close_count == 2);
    assert(g_tcp_rx_len == 8);
    assert(csilk_io_loop_close(&loop) == 0);

    g_rep.tcp_echo_bytes = (int)g_tcp_rx_len;
    g_rep.tcp_close_cbs = g_tcp_close_count;
}

/* ====================================================================
 * Test 4: Full Multi-Worker HTTP/1.1 Server Parity
 * ==================================================================== */

static void
eq_json_handler(csilk_ctx_t* c)
{
    csilk_set_header(c, "Content-Type", "application/json");
    csilk_set_header(c, "X-Backend-Eq", "1");
    csilk_string(c, 200, "{\"equiv\":true}");
}

static void
eq_echo_handler(csilk_ctx_t* c)
{
    size_t      blen = 0;
    const char* body = csilk_get_body(c, &blen);
    char        out[128] = {0};
    if (body && blen > 0) {
        size_t n = blen < sizeof(out) - 1 ? blen : sizeof(out) - 1;
        memcpy(out, body, n);
        out[n] = '\0';
    }
    csilk_string(c, 201, out);
}

static void
eq_stream_handler(csilk_ctx_t* c)
{
    csilk_set_status(c, 200);
    csilk_response_write(c, (const uint8_t*)"part1-", 6);
    csilk_response_write(c, (const uint8_t*)"part2", 5);
    csilk_response_end(c);
}

static void
eq_dispatch_done_cb(csilk_ctx_t* c, void* result)
{
    csilk_string(c, 200, (const char*)result);
    _csilk_send_response(c);
}

static void
eq_dispatch_worker_cb(void* arg)
{
    csilk_async_op_t* op = (csilk_async_op_t*)arg;
    csilk_async_op_complete(op, "dispatched-ok");
}

static void
eq_dispatch_handler(csilk_ctx_t* c)
{
    csilk_async_op_t* op = csilk_async_op_begin(c, 3000, eq_dispatch_done_cb, NULL, NULL);
    assert(op != NULL);
    csilk_dispatch(c, eq_dispatch_worker_cb, op);
}

typedef struct {
    csilk_server_t* server;
    int             port;
} srv_thread_ctx_t;

static void
http_server_thread_fn(void* arg)
{
    srv_thread_ctx_t* sc = (srv_thread_ctx_t*)arg;
    csilk_server_run(sc->server, sc->port);
}

static void
test_http_server_equivalence(void)
{
    csilk_router_t* router = csilk_router_new();
    assert(router != NULL);

    csilk_handler_t h_json[] = {eq_json_handler};
    csilk_handler_t h_echo[] = {eq_echo_handler};
    csilk_handler_t h_stream[] = {eq_stream_handler};
    csilk_handler_t h_dispatch[] = {eq_dispatch_handler};
    assert(csilk_router_add(router, "GET", "/eq/json", h_json, 1) == 0);
    assert(csilk_router_add(router, "POST", "/eq/echo", h_echo, 1) == 0);
    assert(csilk_router_add(router, "GET", "/eq/stream", h_stream, 1) == 0);
    assert(csilk_router_add(router, "GET", "/eq/dispatch", h_dispatch, 1) == 0);

    csilk_server_t* srv = csilk_server_new(router);
    assert(srv != NULL);
    csilk_server_config_t cfg = {
        .worker_threads = 2,
        .idle_timeout_ms = 5000,
        .max_body_size = 1024 * 1024,
        .max_header_size = 64 * 1024,
        .listen_backlog = 64,
    };
    csilk_server_set_config(srv, &cfg);

    int              port = reserve_ephemeral_port();
    srv_thread_ctx_t sctx = {.server = srv, .port = port};
    csilk_thread_t   srv_th;
    assert(csilk_thread_create(&srv_th, http_server_thread_fn, &sctx) == 0);

    /* Wait for listener readiness */
    int probe = -1;
    for (int i = 0; i < 100; i++) {
        probe = tcp_connect_loopback(port);
        if (probe >= 0) {
            close(probe);
            break;
        }
        csilk_io_sleep(10);
    }
    assert(probe >= 0);

    /* 1. GET JSON (keep-alive 4x reuse) */
    int fd = tcp_connect_loopback_retry(port, 20);
    assert(fd >= 0);
    const char* req_get =
        "GET /eq/json HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    for (int i = 0; i < 4; i++) {
        int  status = 0;
        char body[64] = {0};
        assert(send_all(fd, req_get, strlen(req_get)) == 0);
        assert(read_http_response(fd, &status, body, sizeof(body)) == 0);
        assert(status == 200);
        assert(strcmp(body, "{\"equiv\":true}") == 0);
        g_rep.http_get_status = status;
        snprintf(g_rep.http_get_body, sizeof(g_rep.http_get_body), "%s", body);
    }
    g_rep.keepalive_ok = 1;

    /* 2. POST Echo */
    const char* req_post = "POST /eq/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 17\r\n"
                           "Connection: close\r\n\r\npayload=equiv-123";
    assert(send_all(fd, req_post, strlen(req_post)) == 0);
    assert(read_http_response(
               fd, &g_rep.http_post_status, g_rep.http_post_body, sizeof(g_rep.http_post_body)) ==
           0);
    assert(g_rep.http_post_status == 201);
    assert(strcmp(g_rep.http_post_body, "payload=equiv-123") == 0);
    close(fd);

    /* 3. Streaming chunked response */
    fd = tcp_connect_loopback_retry(port, 20);
    assert(fd >= 0);
    const char* req_stream =
        "GET /eq/stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    int stream_status = 0;
    assert(send_all(fd, req_stream, strlen(req_stream)) == 0);
    assert(read_http_response(
               fd, &stream_status, g_rep.http_stream_body, sizeof(g_rep.http_stream_body)) == 0);
    assert(stream_status == 200);
    assert(strcmp(g_rep.http_stream_body, "part1-part2") == 0);
    close(fd);

    /* 4. Cross-worker csilk_dispatch() */
    fd = tcp_connect_loopback_retry(port, 20);
    assert(fd >= 0);
    const char* req_disp =
        "GET /eq/dispatch HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    assert(send_all(fd, req_disp, strlen(req_disp)) == 0);
    assert(read_http_response(fd,
                              &g_rep.http_dispatch_status,
                              g_rep.http_dispatch_body,
                              sizeof(g_rep.http_dispatch_body)) == 0);
    assert(g_rep.http_dispatch_status == 200);
    assert(strcmp(g_rep.http_dispatch_body, "dispatched-ok") == 0);
    close(fd);

    /* 5. 404 Not Found */
    fd = tcp_connect_loopback_retry(port, 20);
    assert(fd >= 0);
    const char* req_404 =
        "GET /eq/missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char body404[64] = {0};
    assert(send_all(fd, req_404, strlen(req_404)) == 0);
    assert(read_http_response(fd, &g_rep.http_404_status, body404, sizeof(body404)) == 0);
    assert(g_rep.http_404_status == 404);
    close(fd);

    csilk_server_stop(srv);
    assert(csilk_thread_join(&srv_th) == 0);
    csilk_server_free(srv);
    csilk_router_free(router);
}

int
main(int argc, char** argv)
{
    const char* emit_json = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--emit-json") == 0) {
            emit_json = argv[i + 1];
        }
    }

#ifdef CSILK_USE_URING
    g_rep.backend = "io_uring";
#else
    g_rep.backend = "libuv";
#endif

    printf("=== Running libuv vs io_uring Equivalence Suite (%s) ===\n", g_rep.backend);
    test_timer_equivalence();
    test_async_equivalence();
    test_tcp_loopback_equivalence();
    test_http_server_equivalence();

    if (emit_json) {
        FILE* f = fopen(emit_json, "w");
        assert(f != NULL);
        fprintf(f,
                "{\n"
                "  \"backend\": \"%s\",\n"
                "  \"timer_oneshot_count\": %d,\n"
                "  \"timer_repeat_count\": %d,\n"
                "  \"timer_close_data_ok\": %d,\n"
                "  \"async_wakeups\": %d,\n"
                "  \"async_close_data_ok\": %d,\n"
                "  \"tcp_echo_bytes\": %d,\n"
                "  \"tcp_close_cbs\": %d,\n"
                "  \"http_get_status\": %d,\n"
                "  \"http_get_body\": %s,\n"
                "  \"http_post_status\": %d,\n"
                "  \"http_post_body\": \"%s\",\n"
                "  \"http_404_status\": %d,\n"
                "  \"http_stream_body\": \"%s\",\n"
                "  \"http_dispatch_status\": %d,\n"
                "  \"http_dispatch_body\": \"%s\",\n"
                "  \"keepalive_ok\": %d\n"
                "}\n",
                g_rep.backend,
                g_rep.timer_oneshot_count,
                g_rep.timer_repeat_count,
                g_rep.timer_close_data_ok,
                g_rep.async_wakeups,
                g_rep.async_close_data_ok,
                g_rep.tcp_echo_bytes,
                g_rep.tcp_close_cbs,
                g_rep.http_get_status,
                "\"{\\\"equiv\\\":true}\"",
                g_rep.http_post_status,
                g_rep.http_post_body,
                g_rep.http_404_status,
                g_rep.http_stream_body,
                g_rep.http_dispatch_status,
                g_rep.http_dispatch_body,
                g_rep.keepalive_ok);
        fclose(f);
    }

    printf("=== %s Equivalence Suite PASSED ===\n", g_rep.backend);
    return 0;
}
