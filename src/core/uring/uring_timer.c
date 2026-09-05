/**
 * @file uring_timer.c
 * @brief Timer operations: start, stop, again.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <stdatomic.h>
#include "uring_internal.h"

static void
csilk_io_timer_op_complete(csilk_io_op_t* op, int status)
{
    (void)status;
    csilk_io_timer_op_t* timer_op = (csilk_io_timer_op_t*)op;
    if (!timer_op || !timer_op->handle) {
        return;
    }

    csilk_io_timer_t* handle = timer_op->handle;
    csilk_io_timer_cb cb = handle->cb;
    if (handle->repeat > 0) {
        struct __kernel_timespec ts;
        ts.tv_sec = (__kernel_time64_t)(handle->repeat / 1000);
        ts.tv_nsec = (long long)(handle->repeat % 1000) * 1000000LL;
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&handle->loop->ring);
        if (sqe) {
            io_uring_prep_timeout(sqe, &ts, 0, 0);
            io_uring_sqe_set_data64(sqe, uring_encode_timer_data(URING_OP_TMR_GENERIC, handle));
            io_uring_submit(&handle->loop->ring);
        }
    } else {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }

    if (cb) {
        cb(handle);
    }
}

static void
csilk_io_timer_op_cancel(csilk_io_op_t* op)
{
    (void)op;
    /* No-op on purpose: csilk_io_timer_stop() already deactivated the handle
     * and dropped the loop's active_handles reference before calling
     * csilk_io_op_cancel().  Counting the reference here as well decremented
     * it twice per stopped timer, net-draining one count per one-shot timer
     * cycle.  After enough cycles the loop's count reached zero while the
     * listen poll was still armed, so the event loop exited and the server
     * stopped accepting connections. */
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

    csilk_io_timer_op_t* timer_op = calloc(1, sizeof(*timer_op));
    if (!timer_op) {
        return -1;
    }

    /* Restarting an active timer overwrites handle->op; the previous wrapper
     * is pure bookkeeping (its state machine is never driven for timers), so
     * release it instead of leaking one struct per restart. */
    if (handle->op) {
        free(handle->op);
        handle->op = NULL;
    }

    csilk_io_op_init(&timer_op->op, CSILK_IO_OP_TIMER, handle, handle->generation);
    timer_op->op.complete = csilk_io_timer_op_complete;
    timer_op->op.cancel = csilk_io_timer_op_cancel;
    timer_op->handle = handle;
    handle->op = timer_op;

    if (csilk_io_op_submit(&timer_op->op) != 0) {
        handle->op = NULL;
        free(timer_op);
        return -1;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        csilk_io_op_cancel(&timer_op->op);
        csilk_io_op_retire(&timer_op->op);
        handle->op = NULL;
        free(timer_op);
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

    if (handle->op) {
        /* Timer ops are bookkeeping only; release the wrapper. */
        free(handle->op);
        handle->op = NULL;
    }

    handle->generation++;
    handle->cb = NULL;

    /* No kernel-side io_uring_prep_cancel64 here: it would require the exact
     * user_data of the original timeout SQE (a different op-context pointer
     * that is no longer available), so the cancel could never match and the
     * freshly allocated cancel-key context would leak on every stop.  The
     * generation bump above already makes any late timeout CQE stale, and
     * is_stale_timer() drops it in the completion loop. */
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

#endif
