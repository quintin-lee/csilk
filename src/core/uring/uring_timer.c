/**
 * @file uring_timer.c
 * @brief Timer operations: start, stop, again.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <stdatomic.h>
#include "uring_internal.h"

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

#endif
