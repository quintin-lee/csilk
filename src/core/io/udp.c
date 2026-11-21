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
#include <liburing.h>
#include "../uring/uring_internal.h"

/* ====================================================================
 * Internal timer callback: level-triggered recvfrom poll (legacy / fallback)
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

    struct sockaddr_storage from;
    memset(&from, 0, sizeof(from));
    socklen_t fromlen = sizeof(from);
    ssize_t   n = recvfrom(handle->fd,
                         handle->recv_buf,
                         sizeof(handle->recv_buf),
                         0,
                         (struct sockaddr*)&from,
                         &fromlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        csilk_io_udp_recv_stop(handle);
        return;
    }
    if (n == 0) {
        return;
    }

    memcpy(&handle->remote_storage, &from, sizeof(struct sockaddr_storage));
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
    /* Create the socket if not already open.  Use the address family from
     * the bind target so the socket matches (AF_INET or AF_INET6). */
    if (handle->fd < 0) {
        int family = AF_INET;
        if (addr && addr->sa_family == AF_INET6) {
            family = AF_INET6;
        }
        int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (family == AF_INET6) {
            /* Allow dual-stack: accept both IPv4-mapped and native IPv6. */
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
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

    /* Submit a multishot IORING_OP_RECVMSG: the kernel arms the SQE once
     * and completes one CQE per datagram without re-submission.  This
     * eliminates the 1 ms timer poll (which capped throughput at ~1 Kpps)
     * and delivers packets as they arrive in the socket buffer.
     *
     * Falls back to the 1 ms timer if the ring does not support multishot
     * (io_uring < 5.6) or if the SQE submission fails. */
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &handle->remote_storage;
    msg.msg_namelen = sizeof(handle->remote_storage);
    struct iovec iov;
    iov.iov_base = handle->recv_buf;
    iov.iov_len = sizeof(handle->recv_buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Control buffer for the sender address (required for recvmsg on UDP). */
    uint8_t ctrl_buf[sizeof(struct sockaddr_storage) + 16];
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    uint64_t sqe_data = uring_encode_handle_data(URING_OP_UDP_RECV, (csilk_io_handle_t*)handle);
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (sqe && sqe_data) {
        io_uring_prep_recvmsg_multishot(sqe, handle->fd, &msg, 0);
        io_uring_sqe_set_data64(sqe, sqe_data);
        io_uring_submit(&loop->ring);
    } else {
        /* Fallback: 1 ms timer-driven recvfrom (legacy path). */
        if (sqe_data) {
            uring_op_free(loop, (uring_op_context_t*)sqe_data);
        }
        int rc = csilk_io_timer_start(&handle->recv_timer, _udp_recv_timer_cb, 1, 1);
        if (rc != 0) {
            handle->cb = NULL;
            return rc;
        }
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
        /* Cancel the in-flight multishot recvmsg SQE so the kernel stops
         * delivering CQEs for this fd. */
        csilk_io_loop_t* loop = handle->loop ? handle->loop : csilk_io_default_loop();
        if (loop) {
            struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
            if (sqe) {
                uint64_t sqe_data =
                    uring_encode_handle_data(URING_OP_UDP_RECV, (csilk_io_handle_t*)handle);
                if (sqe_data) {
                    io_uring_prep_cancel64(sqe, sqe_data, 0);
                    io_uring_sqe_set_data64(sqe, 0);
                    io_uring_submit(&loop->ring);
                }
            }
        }
        /* Also stop the legacy timer in case the fallback path was taken. */
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
    msg.msg_iovlen = limit;

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
