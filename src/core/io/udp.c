/**
 * @file udp.c
 * @brief UDP datagram handle implementation.
 *
 * Provides `csilk_io_udp_*` primitives for QUIC/HTTP3 transports.
 *
 * Under the io_uring backend (`CSILK_USE_URING`), the handle owns a
 * non-blocking datagram socket plus a repeating timer that drives
 * level-triggered `recvfrom()` polls on the loop thread;
 * `csilk_io_udp_send()` calls `sendto()` / `sendmsg()` synchronously.
 *
 * Under the libuv backend, the inline wrappers in `csilk/core/sys_io.h`
 * delegate to `uv_udp_*`; this file only compiles its io_uring half when
 * `CSILK_USE_URING` is set, so the libuv build pulls no extra TU.
 *
 * @copyright MIT License
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ====================================================================
 * Internal timer callback: level-triggered recvfrom poll
 * ==================================================================== */

static void
_udp_recv_timer_cb(csilk_io_timer_t* timer)
{
    /* The timer is owned by csilk_io_udp_t->recv_timer; recover the UDP
     * handle from the timer's data field (set at arming time). */
    csilk_io_udp_t* handle = (csilk_io_udp_t*)timer->data;
    if (!handle || !handle->cb) {
        return;
    }
    if (handle->fd < 0) {
        return;
    }

    struct sockaddr_in from;
    socklen_t          fromlen = sizeof(from);
    ssize_t            n = recvfrom(handle->fd,
                         handle->recv_buf,
                         sizeof(handle->recv_buf),
                         0,
                         (struct sockaddr*)&from,
                         &fromlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No datagram ready — keep the timer armed for the next tick. */
            return;
        }
        /* Permanent error; stop the poll timer. */
        csilk_io_udp_recv_stop(handle);
        return;
    }
    if (n == 0) {
        return;
    }

    /* Cache the remote address for subsequent sends. */
    handle->remote = from;
    handle->remote_len = (int)fromlen;

    csilk_io_buf_t csbuf;
    csbuf.base = handle->recv_buf;
    csbuf.len = (size_t)n;
    handle->cb(handle, (size_t)n, &csbuf, (const struct sockaddr*)&from);
}

/* ====================================================================
 * UDP handle lifecycle
 * ==================================================================== */

int
csilk_io_udp_init(csilk_io_loop_t* loop, csilk_io_udp_t* handle)
{
    if (!handle) {
        return -1;
    }
    uint64_t gen = handle->generation ? handle->generation + 1 : 1;
    memset(handle, 0, sizeof(*handle));
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return -1;
        }
    }
    handle->loop = loop;
    handle->fd = -1;
    handle->flags = 0;
    handle->type = CSILK_IO_HANDLE_UDP;
    handle->close_cb = NULL;
    handle->generation = gen ? gen : 1;
    handle->cb = NULL;
    handle->recv_started = 0;
    handle->remote_len = 0;
    memset(&handle->remote, 0, sizeof(handle->remote));

    /* Initialize the embedded timer handle. */
    int rc = csilk_io_timer_init(loop, &handle->recv_timer);
    if (rc != 0) {
        return rc;
    }
    handle->recv_timer.data = handle;

    return 0;
}

int
csilk_io_udp_open(csilk_io_udp_t* handle, csilk_io_os_sock_t sock)
{
    if (!handle) {
        return -1;
    }
    handle->fd = sock;
    return 0;
}

int
csilk_io_udp_bind(csilk_io_udp_t* handle, const struct sockaddr* addr)
{
    if (!handle) {
        return -1;
    }
    /* Create the socket if not already open. */
    if (handle->fd < 0) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        handle->fd = fd;
    }
    if (addr) {
        socklen_t addrlen = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                          : sizeof(struct sockaddr_in);
        int       rc = bind(handle->fd, addr, addrlen);
        if (rc < 0) {
            close(handle->fd);
            handle->fd = -1;
            return -1;
        }
    }
    return 0;
}

int
csilk_io_udp_recv_start(csilk_io_udp_t* handle, csilk_io_udp_recv_cb cb)
{
    if (!handle || !cb || handle->fd < 0) {
        return -1;
    }
    handle->cb = cb;

    csilk_io_loop_t* loop = handle->loop ? handle->loop : csilk_io_default_loop();
    if (!loop) {
        return -1;
    }
    handle->loop = loop;

    /* Arm a 1 ms repeating timer that drives level-triggered recvfrom.
     * The H3 engine's QUIC timer mechanism may call csilk_io_udp_recv_stop
     * + csilk_io_udp_recv_start to re-arm the poll when its own schedule
     * changes; callers that drive the socket from their own tick may skip
     * the timer entirely by calling csilk_io_udp_send/recvfrom manually. */
    int rc = csilk_io_timer_start(&handle->recv_timer, _udp_recv_timer_cb, 1, 1);
    if (rc != 0) {
        handle->cb = NULL;
        return rc;
    }

    handle->recv_started = 1;
    if (!(handle->flags & CSILK_IO_HANDLE_ACTIVE)) {
        handle->flags |= CSILK_IO_HANDLE_ACTIVE;
        loop->active_handles++;
    }
    handle->generation++;
    return 0;
}

int
csilk_io_udp_recv_stop(csilk_io_udp_t* handle)
{
    if (!handle) {
        return -1;
    }
    if (handle->recv_started) {
        csilk_io_timer_stop(&handle->recv_timer);
        handle->recv_started = 0;
    }
    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->cb = NULL;
    handle->generation++;
    return 0;
}

int
csilk_io_udp_send(csilk_io_udp_t*        handle,
                  void*                  req,
                  const csilk_io_buf_t   bufs[],
                  unsigned int           nbufs,
                  const struct sockaddr* addr)
{
    (void)req; /* libuv passes a uv_udp_send_t* here; ignored under io_uring. */
    if (!handle || !bufs || nbufs == 0) {
        return -1;
    }
    if (handle->fd < 0) {
        return -1;
    }

    /* Single-buffer fast path: plain sendto. */
    if (nbufs == 1) {
        const struct sockaddr* dest;
        socklen_t              destlen;
        if (addr) {
            dest = addr;
            destlen = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                    : sizeof(struct sockaddr_in);
        } else {
            if (handle->remote_len <= 0) {
                return -1;
            }
            dest = (const struct sockaddr*)&handle->remote;
            destlen = handle->remote_len;
        }
        ssize_t sent = sendto(handle->fd, bufs[0].base, bufs[0].len, 0, dest, destlen);
        return (int)sent;
    }

    /* Multi-buffer path: sendmsg with iovec array (max 32 fragments). */
    struct msghdr msg;
    struct iovec  iov[32];
    memset(&msg, 0, sizeof(msg));
    memset(iov, 0, sizeof(iov));
    unsigned int limit = nbufs < 32 ? nbufs : 32;
    for (unsigned int i = 0; i < limit; i++) {
        iov[i].iov_base = bufs[i].base;
        iov[i].iov_len = bufs[i].len;
    }
    msg.msg_iov = iov;
    msg.msg_iovlen = (unsigned int)limit;

    if (addr) {
        msg.msg_name = (struct sockaddr*)addr;
        msg.msg_namelen = (addr->sa_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                        : sizeof(struct sockaddr_in);
    } else {
        if (handle->remote_len <= 0) {
            return -1;
        }
        msg.msg_name = (struct sockaddr*)&handle->remote;
        msg.msg_namelen = handle->remote_len;
    }

    ssize_t written = sendmsg(handle->fd, &msg, 0);
    return (int)written;
}

int
csilk_io_udp_close(csilk_io_udp_t* handle)
{
    if (!handle) {
        return -1;
    }
    if (handle->recv_started) {
        csilk_io_timer_stop(&handle->recv_timer);
        handle->recv_started = 0;
    }
    if (handle->fd >= 0) {
        close(handle->fd);
        handle->fd = -1;
    }
    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->generation++;
    return 0;
}

#endif /* CSILK_USE_URING */
