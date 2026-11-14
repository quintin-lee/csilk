/**
 * @file uring_close.c
 * @brief Generic handle close operation.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include "uring_internal.h"

typedef struct {
    csilk_io_op_t      op;
    csilk_io_handle_t* handle;
    csilk_io_close_cb  cb;
} csilk_io_close_op_t;

static void
csilk_io_close_op_complete(csilk_io_op_t* op, int status)
{
    (void)status;
    csilk_io_close_op_t* close_op = (csilk_io_close_op_t*)op;
    csilk_io_close_cb    cb = close_op ? close_op->cb : NULL;
    if (cb && close_op && close_op->handle) {
        cb(close_op->handle);
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
    } else if (handle->type == CSILK_IO_HANDLE_TCP) {
        csilk_io_tcp_t* tcp = (csilk_io_tcp_t*)handle;
        if (handle->fd >= 0) {
            shutdown(handle->fd, SHUT_WR);
            close(handle->fd);
            handle->fd = -1;
        }
        if (tcp->recv_buf.base) {
            extern void           pool_put_read_buf(worker_pool_t * wp, char* buf, size_t len);
            extern worker_pool_t* _csilk_worker_get_current_pool(void);
            pool_put_read_buf(
                _csilk_worker_get_current_pool(), tcp->recv_buf.base, tcp->recv_buf.len);
            tcp->recv_buf.base = NULL;
            tcp->recv_buf.len = 0;
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
