/**
 * @file uring_handle.c
 * @brief Handle initialization, timer operations, and generic close.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <csilk/csilk.h>
#include <csilk/core/server/server.h>
#include <csilk/core/internal.h>
#include <csilk/core/sync.h>
#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* Pool helpers declared in connection_pool.c / connection_close.c */
extern void           pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);
extern worker_pool_t* _csilk_worker_get_current_pool(void);

typedef struct {
    csilk_io_op_t      op;
    csilk_io_handle_t* handle;
    csilk_io_close_cb  cb;
} csilk_io_close_op_t;

/* ====================================================================
 * Async handle
 * ==================================================================== */

int
csilk_io_async_init(csilk_io_loop_t* loop, csilk_io_async_t* async, csilk_io_async_cb async_cb)
{
    if (!async) {
        return -1;
    }
    /* Explicit field-by-field init avoids a raw memset over the _Atomic(int)
     * event_fd field, which TSan flags as a data race against the atomic
     * load in csilk_io_async_send().  The struct is typically calloc'd by the
     * caller (server_new), so fields are already zero — we only set what we
     * change. */
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    async->loop = loop;
    async->data = NULL;
    async->fd = -1;
    async->flags = 0;
    async->type = CSILK_IO_HANDLE_ASYNC;
    async->close_cb = NULL;
    async->generation = 1;
    async->cb = async_cb;
    int efds = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    atomic_store_explicit(&async->event_fd, efds, memory_order_relaxed);
    async->fd = efds;
    async->flags |= CSILK_IO_HANDLE_ACTIVE;

    if (atomic_load_explicit(&async->event_fd, memory_order_relaxed) < 0) {
        return -1;
    }

    if (loop) {
        loop->active_handles++;
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
        if (sqe) {
            int event_fd_val = atomic_load_explicit(&async->event_fd, memory_order_relaxed);
            io_uring_prep_poll_add(sqe, event_fd_val, POLLIN);
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
    int event_fd_val = atomic_load_explicit(&async->event_fd, memory_order_acquire);
    if (!async || event_fd_val < 0) {
        return -1;
    }
    uint64_t val = 1;
    ssize_t  ret = write(event_fd_val, &val, sizeof(val));
    (void)ret;
    return 0;
}

/* ====================================================================
 * Signal handle
 * ==================================================================== */

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
    csilk_thread_sigmask(SIG_BLOCK, &mask, NULL);

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

/* ====================================================================
 * Timer handle
 * ==================================================================== */

int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
    if (!handle) {
        return -1;
    }
    uint64_t gen = handle->generation ? (handle->generation + 1) : 1;
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

static void
_uring_timer_cancel_inflight(csilk_io_timer_t* handle)
{
    if (!handle || !handle->op) {
        return;
    }

    uring_op_context_t* old_ctx = (uring_op_context_t*)handle->op;
    old_ctx->flags |= URING_OP_CTX_CANCELLED;
    old_ctx->owner = NULL;
    handle->op = NULL;

    csilk_io_loop_t* loop = handle->loop;
    if (!loop) {
        return;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (sqe) {
        io_uring_prep_cancel64(sqe, (uint64_t)(uintptr_t)old_ctx, 0);
        io_uring_sqe_set_data64(sqe, 0);
        io_uring_submit(&loop->ring);
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
        if (!loop) {
            return -1;
        }
        handle->loop = loop;
        handle->ring = &loop->ring;
    }

    _uring_timer_cancel_inflight(handle);

    handle->generation++;
    handle->cb = cb;
    handle->timeout = timeout;
    handle->repeat = repeat;

    struct __kernel_timespec ts;
    ts.tv_sec = (__kernel_time64_t)(timeout / 1000);
    ts.tv_nsec = (long long)(timeout % 1000) * 1000000LL;

    uint64_t sqe_data = uring_encode_timer_data(URING_OP_TMR_GENERIC, handle);
    if (!sqe_data) {
        return -1;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        uring_op_free(loop, (uring_op_context_t*)(uintptr_t)sqe_data);
        return -1;
    }

    if (!(handle->flags & CSILK_IO_HANDLE_ACTIVE)) {
        handle->flags |= CSILK_IO_HANDLE_ACTIVE;
        loop->active_handles++;
    }
    handle->op = (csilk_io_timer_op_t*)(uintptr_t)sqe_data;

    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data64(sqe, sqe_data);
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

    _uring_timer_cancel_inflight(handle);

    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->generation++;
    handle->cb = NULL;
    return 0;
}

int
csilk_io_timer_again(csilk_io_timer_t* handle)
{
    if (!handle) {
        return -1;
    }
    return csilk_io_timer_start(handle, handle->cb, handle->timeout, handle->repeat);
}

/* ====================================================================
 * Poll handle
 * ==================================================================== */

int
csilk_io_poll_init(csilk_io_loop_t* loop, csilk_io_poll_t* handle, int fd)
{
    if (!handle || fd < 0) {
        return -1;
    }
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    handle->loop = loop;
    handle->data = NULL;
    handle->fd = fd;
    handle->flags = 0;
    handle->type = CSILK_IO_HANDLE_POLL;
    handle->close_cb = NULL;
    handle->generation = 1;
    handle->cb = NULL;
    handle->events = 0;
    return 0;
}

int
csilk_io_poll_init_socket(csilk_io_loop_t* loop, csilk_io_poll_t* handle, csilk_io_os_sock_t socket)
{
    return csilk_io_poll_init(loop, handle, (int)socket);
}

int
csilk_io_poll_start(csilk_io_poll_t* handle, int events, csilk_io_poll_cb cb)
{
    if (!handle || handle->fd < 0 || !cb) {
        return -1;
    }
    handle->cb = cb;
    handle->events = events;
    handle->flags |= CSILK_IO_HANDLE_ACTIVE;
    handle->generation++;

    short poll_mask = 0;
    if (events & CSILK_IO_READABLE) {
        poll_mask |= POLLIN | POLLPRI;
    }
    if (events & CSILK_IO_WRITABLE) {
        poll_mask |= POLLOUT;
    }

    csilk_io_loop_t* loop = handle->loop;
    if (loop) {
        loop->active_handles++;
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, handle->fd, poll_mask);
            io_uring_sqe_set_data64(
                sqe, uring_encode_handle_data(URING_OP_POLL, (csilk_io_handle_t*)handle));
            io_uring_submit(&loop->ring);
        }
    }
    return 0;
}

int
csilk_io_poll_stop(csilk_io_poll_t* handle)
{
    if (!handle || !(handle->flags & CSILK_IO_HANDLE_ACTIVE)) {
        return 0;
    }
    handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
    handle->generation++;
    csilk_io_loop_t* loop = handle->loop;
    if (loop && loop->active_handles > 0) {
        loop->active_handles--;
    }
    return 0;
}

/* ====================================================================
 * Generic handle close
 * ==================================================================== */

static void
csilk_io_close_op_complete(csilk_io_op_t* op, int status)
{
    (void)status;
    csilk_io_close_op_t* close_op = (csilk_io_close_op_t*)op;
    csilk_io_handle_t*   handle = close_op ? close_op->handle : NULL;
    csilk_io_close_cb    cb = close_op ? close_op->cb : NULL;
    if (handle && handle->type == CSILK_IO_HANDLE_TCP) {
        csilk_io_tcp_t* tcp = (csilk_io_tcp_t*)handle;
        if (tcp->recv_buf.base) {
            pool_put_read_buf(
                _csilk_worker_get_current_pool(), tcp->recv_buf.base, tcp->recv_buf.len);
            tcp->recv_buf.base = NULL;
            tcp->recv_buf.len = 0;
        }
    }
    if (cb && handle) {
        cb(handle);
    }
}

static void
csilk_io_close_op_cancel(csilk_io_op_t* op)
{
    (void)op;
}

static void
_csilk_uring_deferred_close_cb(csilk_io_work_t* req, int status)
{
    (void)status;
    csilk_io_close_op_t* close_op = (csilk_io_close_op_t*)req;
    if (close_op) {
        csilk_io_op_complete(&close_op->op, 0);
        csilk_io_op_retire(&close_op->op);
        free(close_op);
    }
}

void
csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb)
{
    if (!handle) {
        if (cb) {
            cb(handle);
        }
        return;
    }
    if (handle->flags & CSILK_IO_HANDLE_CLOSING) {
        if (cb) {
            cb(handle);
        }
        return;
    }

    csilk_io_close_op_t* close_op = calloc(1, sizeof(*close_op));
    if (!close_op) {
        if (cb) {
            cb(handle);
        }
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

    csilk_io_op_init(&close_op->op, CSILK_IO_OP_CLOSE, handle, handle->generation);
    close_op->op.complete = csilk_io_close_op_complete;
    close_op->op.cancel = csilk_io_close_op_cancel;
    close_op->handle = handle;
    close_op->cb = cb;
    csilk_io_op_submit(&close_op->op);

    if (handle->type == CSILK_IO_HANDLE_TIMER) {
        csilk_io_timer_stop((csilk_io_timer_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_SIGNAL) {
        csilk_io_signal_stop((csilk_io_signal_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_ASYNC) {
        csilk_io_async_t* async = (csilk_io_async_t*)handle;
        int event_fd_val = atomic_load_explicit(&async->event_fd, memory_order_relaxed);
        if (event_fd_val >= 0) {
            close(event_fd_val);
            atomic_store_explicit(&async->event_fd, -1, memory_order_release);
            async->fd = -1;
        }
    } else if (handle->type == CSILK_IO_HANDLE_POLL) {
        csilk_io_poll_stop((csilk_io_poll_t*)handle);
        handle->fd = -1;
    } else if (handle->type == CSILK_IO_HANDLE_FS_EVENT) {
        csilk_io_fs_event_stop((csilk_io_fs_event_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_TCP) {
        if (handle->fd >= 0) {
            shutdown(handle->fd, SHUT_WR);
            close(handle->fd);
            handle->fd = -1;
        }
    } else if (handle->fd >= 0) {
        shutdown(handle->fd, SHUT_WR);
        close(handle->fd);
        handle->fd = -1;
    }

    if (_uring_deferred_push((csilk_io_work_t*)close_op, _csilk_uring_deferred_close_cb, 0) != 0) {
        csilk_io_op_complete(&close_op->op, 0);
        csilk_io_op_retire(&close_op->op);
        free(close_op);
    }
}

#endif
