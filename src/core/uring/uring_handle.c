/**
 * @file uring_handle.c
 * @brief Handle initialization: async, signal, timer.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <csilk/csilk.h>
#include <csilk/core/server.h>
#include <csilk/core/internal.h>
#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

/* ====================================================================
 * Async handle
 * ==================================================================== */

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

/* ====================================================================
 * Timer handle
 * ==================================================================== */

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

#endif
