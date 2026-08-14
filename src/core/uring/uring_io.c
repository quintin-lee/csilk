/**
 * @file uring_io.c
 * @brief io_uring driver implementation for the csilk_io abstraction.
 *
 * Implements the csilk_io surface (handles, loop, timer, tcp, stream, signal,
 * async, and write pipeline) backed by io_uring.
 *
 * @copyright MIT License
 */

#include <csilk/core/sys_io.h>

#ifdef CSILK_USE_URING

#include <csilk/csilk.h>
#include <csilk/core/server.h>
#include <csilk/core/internal.h>
#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <poll.h>

/* Forward declarations */
static csilk_io_loop_t  g_default_loop;
static int              g_default_loop_inited = 0;
static int              g_default_pending = 0;
static struct io_uring* g_default_ring_ptr = NULL;

int
csilk_io_loop_init(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    memset(loop, 0, sizeof(*loop));
    return io_uring_queue_init(4096, &loop->ring, 0);
}

int
csilk_io_loop_close(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    io_uring_queue_exit(&loop->ring);
    if (loop == &g_default_loop) {
        g_default_loop_inited = 0;
        g_default_ring_ptr = NULL;
    }
    return 0;
}

void
csilk_io_stop(csilk_io_loop_t* loop)
{
    if (loop) {
        loop->stop_flag = 1;
    }
}

uint64_t
csilk_io_now(const csilk_io_loop_t* loop)
{
    (void)loop;
    return csilk_io_hrtime() / 1000000ULL;
}

void
csilk_io_update_time(csilk_io_loop_t* loop)
{
    (void)loop;
}

int
csilk_io_tcp_init(csilk_io_loop_t* loop, csilk_io_tcp_t* handle)
{
    if (!handle) {
        return -1;
    }
    uint8_t gen = handle->generation ? (uint8_t)(handle->generation + 1) : 1;
    memset(handle, 0, sizeof(*handle));
    handle->loop = loop;
    handle->fd = -1;
    handle->type = CSILK_IO_HANDLE_TCP;
    handle->generation = gen ? gen : 1;
    return 0;
}

int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
    if (!async) {
        return -1;
    }
    memset(async, 0, sizeof(*async));
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    async->loop = loop;
    async->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    async->fd = async->event_fd;
    async->cb = async_cb;
    async->type = CSILK_IO_HANDLE_ASYNC;
    async->flags |= CSILK_IO_HANDLE_ACTIVE;
    async->generation = 1;

    if (async->event_fd < 0) {
        return -1;
    }

    if (loop) {
        loop->active_handles++;
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, async->event_fd, POLLIN);
            io_uring_sqe_set_data64(
                sqe, uring_encode_handle_data(URING_OP_POLL_ASYNC, (csilk_io_handle_t*)async));
            io_uring_submit(&loop->ring);
        }
    }
    return 0;
}

int
csilk_io_async_send(csilk_io_async_t* async)
{
    if (!async || async->event_fd < 0) {
        return -1;
    }
    uint64_t val = 1;
    ssize_t  ret = write(async->event_fd, &val, sizeof(val));
    (void)ret;
    return 0;
}

int
csilk_io_signal_init(csilk_io_loop_t* loop, csilk_io_signal_t* handle)
{
    if (!handle) {
        return -1;
    }
    memset(handle, 0, sizeof(*handle));
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    handle->loop = loop;
    handle->signal_fd = -1;
    handle->fd = -1;
    handle->type = CSILK_IO_HANDLE_SIGNAL;
    handle->generation = 1;
    return 0;
}

int
csilk_io_signal_start(csilk_io_signal_t* handle, csilk_io_signal_cb cb, int signum)
{
    if (!handle || signum <= 0) {
        return -1;
    }
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(handle->signal_fd, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        return -1;
    }
    handle->signal_fd = sfd;
    handle->fd = sfd;
    handle->cb = cb;
    handle->signum = signum;
    handle->flags |= CSILK_IO_HANDLE_ACTIVE;
    handle->generation++;

    csilk_io_loop_t* loop = handle->loop ? handle->loop : csilk_io_default_loop();
    handle->loop = loop;
    loop->active_handles++;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (sqe) {
        io_uring_prep_poll_add(sqe, handle->signal_fd, POLLIN);
        io_uring_sqe_set_data64(
            sqe, uring_encode_handle_data(URING_OP_POLL_SIGNAL, (csilk_io_handle_t*)handle));
        io_uring_submit(&loop->ring);
    }
    return 0;
}

int
csilk_io_signal_stop(csilk_io_signal_t* handle)
{
    if (!handle || handle->signal_fd < 0) {
        return -1;
    }
    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->generation++;
    close(handle->signal_fd);
    handle->signal_fd = -1;
    handle->fd = -1;
    return 0;
}

int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
    if (!handle) {
        return -1;
    }
    uint8_t gen = handle->generation ? (uint8_t)(handle->generation + 1) : 1;
    memset(handle, 0, sizeof(*handle));
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    handle->loop = loop;
    handle->ring = loop ? &loop->ring : NULL;
    handle->fd = -1;
    handle->type = CSILK_IO_HANDLE_TIMER;
    handle->generation = gen ? gen : 1;
    return 0;
}

int
csilk_io_tcp_open(csilk_io_tcp_t* handle, csilk_io_os_sock_t sock)
{
    if (!handle) {
        return -1;
    }
    handle->fd = sock;
    return 0;
}

int
csilk_io_tcp_bind(csilk_io_tcp_t* handle, const struct sockaddr* addr, unsigned int flags)
{
    (void)flags;
    if (!handle || !addr) {
        return -1;
    }
    if (handle->fd < 0) {
        int family = addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            return -1;
        }
        int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
        handle->fd = fd;
    }
    socklen_t addrlen =
        (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    int rc = bind(handle->fd, addr, addrlen);
    if (rc < 0) {
        close(handle->fd);
        handle->fd = -1;
        return -1;
    }
    return 0;
}

int
csilk_io_listen(csilk_io_stream_t* stream, int backlog, csilk_io_connection_cb cb)
{
    if (!stream || stream->fd < 0) {
        return -1;
    }
    if (listen(stream->fd, backlog) < 0) {
        return -1;
    }
    stream->connection_cb = cb;
    stream->flags |= CSILK_IO_HANDLE_ACTIVE;
    stream->generation++;

    csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();
    stream->loop = loop;
    loop->active_handles++;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_poll_add(sqe, stream->fd, POLLIN);
    io_uring_sqe_set_data64(
        sqe, uring_encode_handle_data(URING_OP_POLL_LISTEN, (csilk_io_handle_t*)stream));
    io_uring_submit(&loop->ring);
    return 0;
}

int
csilk_io_accept(csilk_io_stream_t* server, csilk_io_stream_t* client)
{
    if (!server || server->fd < 0 || !client) {
        return -1;
    }
    int fd = accept4(server->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    client->fd = fd;
    client->loop = server->loop;
    return 0;
}

int
csilk_io_tcp_nodelay(csilk_io_tcp_t* handle, int enable)
{
    if (!handle || handle->fd < 0) {
        return -1;
    }
    return setsockopt(handle->fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
}

int
csilk_io_tcp_keepalive(csilk_io_tcp_t* handle, int enable, unsigned int delay)
{
    if (!handle || handle->fd < 0) {
        return -1;
    }
    (void)delay;
    return setsockopt(handle->fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
}

int
csilk_io_tcp_getpeername(const csilk_io_tcp_t* handle, struct sockaddr* name, int* namelen)
{
    if (!handle || handle->fd < 0 || !name || !namelen) {
        return -1;
    }
    socklen_t slen = (socklen_t)*namelen;
    int       r = getpeername(handle->fd, name, &slen);
    *namelen = (int)slen;
    return r;
}

int
csilk_io_ip4_addr(const char* ip, int port, struct sockaddr_in* addr)
{
    if (!ip || !addr) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
        return -1;
    }
    return 0;
}

int
csilk_io_ip4_name(const struct sockaddr_in* src, char* dst, size_t size)
{
    if (!src || !dst) {
        return -1;
    }
    return inet_ntop(AF_INET, &src->sin_addr, dst, (socklen_t)size) ? 0 : -1;
}

int
csilk_io_ip6_name(const struct sockaddr_in6* src, char* dst, size_t size)
{
    if (!src || !dst) {
        return -1;
    }
    return inet_ntop(AF_INET6, &src->sin6_addr, dst, (socklen_t)size) ? 0 : -1;
}

int
csilk_io_read_start(csilk_io_stream_t* stream, csilk_io_alloc_cb alloc_cb, csilk_io_read_cb read_cb)
{
    if (!stream || stream->fd < 0) {
        return -1;
    }
    stream->alloc_cb = alloc_cb;
    stream->read_cb = read_cb;
    if (stream->reading && (stream->flags & CSILK_IO_HANDLE_ACTIVE)) {
        return 0;
    }
    stream->reading = 1;
    if (!(stream->flags & CSILK_IO_HANDLE_ACTIVE)) {
        stream->flags |= CSILK_IO_HANDLE_ACTIVE;
        csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();
        stream->loop = loop;
        loop->active_handles++;
    }
    stream->generation++;

    csilk_io_loop_t*     loop = stream->loop ? stream->loop : csilk_io_default_loop();
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }
    io_uring_prep_poll_add(sqe, stream->fd, POLLIN | POLLHUP | POLLERR);
    io_uring_sqe_set_data64(
        sqe, uring_encode_handle_data(URING_OP_POLL_READ, (csilk_io_handle_t*)stream));
    io_uring_submit(&loop->ring);
    return 0;
}

int
csilk_io_read_stop(csilk_io_stream_t* stream)
{
    if (!stream) {
        return -1;
    }
    if (stream->flags & CSILK_IO_HANDLE_ACTIVE) {
        stream->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (stream->loop && stream->loop->active_handles > 0) {
            stream->loop->active_handles--;
        }
    }
    stream->reading = 0;
    stream->generation++;
    return 0;
}

int
csilk_io_write(csilk_io_write_t*    req,
               csilk_io_stream_t*   handle,
               const csilk_io_buf_t bufs[],
               unsigned int         nbufs,
               csilk_io_write_cb    cb)
{
    if (!handle || handle->fd < 0) {
        return -1;
    }
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client && client->ctx.conn_closed) {
        return -1;
    }

    csilk_io_loop_t* loop = handle->loop;
    if (!loop && client && client->owner_pool) {
        loop = client->owner_pool->loop_ptr;
    }
    if (!loop) {
        loop = csilk_io_default_loop();
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }

    req->cb = (void*)cb;
    req->handle = handle;

    struct iovec* iov = NULL;
    if (nbufs == 1) {
        io_uring_prep_send(sqe, handle->fd, bufs[0].base, bufs[0].len, MSG_NOSIGNAL);
    } else {
        iov = malloc(sizeof(struct iovec) * nbufs);
        if (!iov) {
            return -1;
        }
        for (unsigned int i = 0; i < nbufs; ++i) {
            iov[i].iov_base = bufs[i].base;
            iov[i].iov_len = bufs[i].len;
        }
        io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);
    }

    void** ctx = malloc(sizeof(void*) * 3);
    if (!ctx) {
        if (iov) {
            free(iov);
        }
        return -1;
    }
    ctx[0] = client;
    ctx[1] = req;
    ctx[2] = iov;
    io_uring_sqe_set_data64(sqe, uring_encode_data(URING_OP_UV_WRITE, client, ctx));
    if (client) {
        _csilk_ctx_async_ref_incr(&client->ctx);
    }
    io_uring_submit(&loop->ring);
    return 0;
}

void
csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb)
{
    if (!handle) {
        return;
    }
    if (handle->flags & CSILK_IO_HANDLE_CLOSING) {
        return;
    }
    handle->flags |= CSILK_IO_HANDLE_CLOSING;
    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->generation++;

    if (handle->type == CSILK_IO_HANDLE_TIMER) {
        csilk_io_timer_stop((csilk_io_timer_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_SIGNAL) {
        csilk_io_signal_stop((csilk_io_signal_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_ASYNC) {
        csilk_io_async_t* async = (csilk_io_async_t*)handle;
        if (async->event_fd >= 0) {
            close(async->event_fd);
            async->event_fd = -1;
            async->fd = -1;
        }
    } else if (handle->fd >= 0) {
        csilk_client_t* client = (csilk_client_t*)handle->data;
        if (!client || client->async_ref <= 0) {
            close(handle->fd);
            handle->fd = -1;
        }
    }

    if (cb) {
        cb(handle);
    }
}

int
csilk_io_timer_start(csilk_io_timer_t* handle,
                     csilk_io_timer_cb cb,
                     uint64_t          timeout,
                     uint64_t          repeat)
{
    if (!handle) {
        return -1;
    }
    csilk_io_loop_t* loop = handle->loop;
    if (!loop) {
        loop = csilk_io_default_loop();
        handle->loop = loop;
        handle->ring = &loop->ring;
    }

    handle->generation++;
    handle->cb = cb;
    handle->timeout = timeout;
    handle->repeat = repeat;
    if (!(handle->flags & CSILK_IO_HANDLE_ACTIVE)) {
        handle->flags |= CSILK_IO_HANDLE_ACTIVE;
        loop->active_handles++;
    }

    struct __kernel_timespec ts;
    ts.tv_sec = (__kernel_time64_t)(timeout / 1000);
    ts.tv_nsec = (long long)(timeout % 1000) * 1000000LL;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }

    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data64(sqe, uring_encode_timer_data(URING_OP_TMR_GENERIC, handle));
    io_uring_submit(&loop->ring);
    if (loop == &g_default_loop) {
        g_default_pending++;
    }
    return 0;
}

int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
    if (!handle) {
        return -1;
    }
    if (!(handle->flags & CSILK_IO_HANDLE_ACTIVE)) {
        handle->cb = NULL;
        return 0;
    }
    handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
    if (handle->loop && handle->loop->active_handles > 0) {
        handle->loop->active_handles--;
    }
    uint64_t cancel_val = uring_encode_timer_data(URING_OP_TMR_GENERIC, handle);
    handle->generation++;
    handle->cb = NULL;

    csilk_io_loop_t* loop = handle->loop;
    if (!loop) {
        return 0;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (sqe) {
        io_uring_prep_cancel64(sqe, cancel_val, 0);
        io_uring_sqe_set_data64(sqe, 0);
        io_uring_submit(&loop->ring);
    }
    return 0;
}

int
csilk_io_timer_again(csilk_io_timer_t* handle)
{
    if (!handle) {
        return -1;
    }
    if (handle->repeat > 0 && handle->cb) {
        return csilk_io_timer_start(handle, handle->cb, handle->repeat, handle->repeat);
    }
    return 0;
}

csilk_io_loop_t*
csilk_io_default_loop(void)
{
    if (!g_default_loop_inited) {
        g_default_loop_inited = 1;
        if (csilk_io_loop_init(&g_default_loop) != 0) {
            return NULL;
        }
        g_default_ring_ptr = &g_default_loop.ring;
    }
    return &g_default_loop;
}

int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return -1;
        }
    }

    struct io_uring* ring = &loop->ring;
    int              total = 0;
    loop->stop_flag = 0;

    do {
        /* 1. Drain deferred after-work callbacks (iteratively). */
        int n;
        while ((n = _uring_deferred_drain_all()) > 0) {
            total += n;
        }

        if (loop->stop_flag) {
            break;
        }

        /* 2. Wait for / peek CQEs */
        struct io_uring_cqe* cqe = NULL;
        unsigned             head;
        unsigned             cq_count = 0;

        if (mode == CSILK_IO_RUN_NOWAIT) {
            int ret = io_uring_peek_cqe(ring, &cqe);
            if (ret != 0 || !cqe) {
                break;
            }
        } else if (mode == CSILK_IO_RUN_ONCE) {
            struct __kernel_timespec ts = {0, 10000000}; /* 10 ms */
            io_uring_wait_cqe_timeout(ring, &cqe, &ts);
        } else {
            /* CSILK_IO_RUN_DEFAULT */
            if (loop->active_handles <= 0) {
                break;
            }
            struct __kernel_timespec ts = {0, 50000000}; /* 50 ms timeout */
            io_uring_wait_cqe_timeout(ring, &cqe, &ts);
        }

        /* 3. Process all ready CQEs */
        io_uring_for_each_cqe(ring, head, cqe)
        {
            cq_count++;
            total++;

            int             res = cqe->res;
            uint8_t         gen = 0;
            void*           ptr = NULL;
            uring_op_type_t op = URING_OP_NONE;
            uring_decode_data(cqe->user_data, &op, &ptr, &gen);

            if (op == URING_OP_POLL_LISTEN) {
                csilk_io_stream_t* s = (csilk_io_stream_t*)ptr;
                if (s && s->fd >= 0 && (s->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(s->flags & CSILK_IO_HANDLE_CLOSING)) {
                    /* Re-arm listen poll */
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, s->fd, POLLIN);
                        io_uring_sqe_set_data64(
                            sqe,
                            uring_encode_handle_data(URING_OP_POLL_LISTEN, (csilk_io_handle_t*)s));
                        io_uring_submit(ring);
                    }
                    if (res >= 0 && s->connection_cb) {
                        s->connection_cb(s, 0);
                    }
                }
            } else if (op == URING_OP_POLL_READ) {
                csilk_io_stream_t* s = (csilk_io_stream_t*)ptr;
                if (s && s->fd >= 0 && s->reading && (s->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(s->flags & CSILK_IO_HANDLE_CLOSING) && s->generation == gen) {
                    if (res < 0 && res != -ECANCELED) {
                        if (s->read_cb) {
                            csilk_io_buf_t buf = {NULL, 0};
                            s->read_cb(s, res, &buf);
                        }
                    } else if (s->alloc_cb && s->read_cb) {
                        csilk_io_buf_t buf;
                        s->alloc_cb((csilk_io_handle_t*)s, 65536, &buf);
                        if (buf.base && buf.len > 0) {
                            ssize_t nread = read(s->fd, buf.base, buf.len);
                            if (nread > 0) {
                                s->read_cb(s, nread, &buf);
                                /* Re-arm poll if still reading */
                                if (s->reading && s->fd >= 0 &&
                                    !(s->flags & CSILK_IO_HANDLE_CLOSING)) {
                                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                                    if (sqe) {
                                        io_uring_prep_poll_add(
                                            sqe, s->fd, POLLIN | POLLHUP | POLLERR);
                                        io_uring_sqe_set_data64(
                                            sqe,
                                            uring_encode_handle_data(URING_OP_POLL_READ,
                                                                     (csilk_io_handle_t*)s));
                                        io_uring_submit(ring);
                                    }
                                }
                            } else if (nread == 0) {
                                s->reading = 0;
                                s->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                                if (loop->active_handles > 0) {
                                    loop->active_handles--;
                                }
                                s->read_cb(s, -4095 /* UV_EOF */, &buf);
                            } else {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    if (buf.base) {
                                        free(buf.base);
                                    }
                                    if (s->reading && s->fd >= 0 &&
                                        !(s->flags & CSILK_IO_HANDLE_CLOSING)) {
                                        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                                        if (sqe) {
                                            io_uring_prep_poll_add(
                                                sqe, s->fd, POLLIN | POLLHUP | POLLERR);
                                            io_uring_sqe_set_data64(
                                                sqe,
                                                uring_encode_handle_data(URING_OP_POLL_READ,
                                                                         (csilk_io_handle_t*)s));
                                            io_uring_submit(ring);
                                        }
                                    }
                                } else {
                                    s->reading = 0;
                                    s->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                                    if (loop->active_handles > 0) {
                                        loop->active_handles--;
                                    }
                                    s->read_cb(s, -errno, &buf);
                                }
                            }
                        }
                    }
                }
            } else if (op == URING_OP_POLL_ASYNC) {
                csilk_io_async_t* async = (csilk_io_async_t*)ptr;
                if (async && async->event_fd >= 0 && (async->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(async->flags & CSILK_IO_HANDLE_CLOSING)) {
                    uint64_t val = 0;
                    ssize_t  nr = read(async->event_fd, &val, sizeof(val));
                    /* Re-arm poll */
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, async->event_fd, POLLIN);
                        io_uring_sqe_set_data64(
                            sqe,
                            uring_encode_handle_data(URING_OP_POLL_ASYNC,
                                                     (csilk_io_handle_t*)async));
                        io_uring_submit(ring);
                    }
                    if (nr > 0 && async->cb) {
                        async->cb(async);
                    }
                }
            } else if (op == URING_OP_POLL_SIGNAL) {
                csilk_io_signal_t* sig = (csilk_io_signal_t*)ptr;
                if (sig && sig->signal_fd >= 0 && (sig->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(sig->flags & CSILK_IO_HANDLE_CLOSING)) {
                    struct signalfd_siginfo fdsi;
                    ssize_t                 nr = read(sig->signal_fd, &fdsi, sizeof(fdsi));
                    /* Re-arm signal poll */
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, sig->signal_fd, POLLIN);
                        io_uring_sqe_set_data64(sqe,
                                                uring_encode_handle_data(URING_OP_POLL_SIGNAL,
                                                                         (csilk_io_handle_t*)sig));
                        io_uring_submit(ring);
                    }
                    if (nr > 0 && sig->cb) {
                        sig->cb(sig, sig->signum);
                    }
                }
            } else if (op == URING_OP_UV_WRITE) {
                csilk_uv_on_write_done(ptr, res);
            } else if (op == URING_OP_TMR_GENERIC) {
                csilk_io_timer_t* tmr = (csilk_io_timer_t*)ptr;
                if (tmr && tmr->generation == gen && (tmr->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(tmr->flags & CSILK_IO_HANDLE_CLOSING) && tmr->cb) {
                    csilk_io_timer_cb cb = tmr->cb;
                    if (tmr->repeat > 0) {
                        /* Re-arm repeating timer */
                        struct __kernel_timespec ts;
                        ts.tv_sec = (__kernel_time64_t)(tmr->repeat / 1000);
                        ts.tv_nsec = (long long)(tmr->repeat % 1000) * 1000000LL;
                        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                        if (sqe) {
                            io_uring_prep_timeout(sqe, &ts, 0, 0);
                            io_uring_sqe_set_data64(
                                sqe, uring_encode_timer_data(URING_OP_TMR_GENERIC, tmr));
                            io_uring_submit(ring);
                        }
                    } else {
                        tmr->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                        if (tmr->loop && tmr->loop->active_handles > 0) {
                            tmr->loop->active_handles--;
                        }
                    }
                    cb(tmr);
                }
            }
        }

        if (cq_count > 0) {
            io_uring_cq_advance(ring, cq_count);
            if (loop == &g_default_loop) {
                g_default_pending -= (int)cq_count;
                if (g_default_pending < 0) {
                    g_default_pending = 0;
                }
            }
            while ((n = _uring_deferred_drain_all()) > 0) {
                total += n;
            }
        }

        if (mode == CSILK_IO_RUN_NOWAIT || mode == CSILK_IO_RUN_ONCE) {
            break;
        }

    } while (!loop->stop_flag && loop->active_handles > 0);

    return total > 0 ? 0 : -1;
}

#endif

void
csilk_uv_on_write_done(void* arg, ssize_t res)
{
    void** ctx = (void**)arg;
    if (!ctx) {
        return;
    }
    csilk_client_t*   client = (csilk_client_t*)ctx[0];
    csilk_io_write_t* req = (csilk_io_write_t*)ctx[1];
    struct iovec*     iov = (struct iovec*)ctx[2];

    if (iov) {
        free(iov);
    }
    if (req && req->cb) {
        csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
        cb(req, (int)res);
    }
    free(ctx);

    if (client) {
        _csilk_ctx_async_ref_decr(&client->ctx);
    }
}
