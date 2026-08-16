/**
 * @file uring_timer.c
 * @brief Timer operations: start, stop, again.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include "uring_internal.h"

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

#endif
