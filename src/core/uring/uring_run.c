/**
 * @file uring_run.c
 * @brief io_uring event loop dispatch: csilk_io_run and write_done callback.
 *
 * Contains the CQE polling loop with handlers for all operation types
 * (listen, read, async, signal, write, timer).
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <errno.h>
#include <poll.h>
#include <sys/signalfd.h>

#include "../internal/srv_internal.h"
#include "uring_internal.h"

/* Forward declarations for functions defined in server modules */
extern void on_read(csilk_client_t* client, ssize_t nread);
extern void on_timeout(csilk_client_t* client);
extern void client_destroy(csilk_client_t* client);
extern void csilk_client_close(csilk_client_t* client);
extern void on_close_done(csilk_client_t* client);

extern void pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);

/* ====================================================================
 * Event loop
 * ==================================================================== */

int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return -1;
        }
    }

    if (loop->running) {
        /* Avoid recursive event loop invocation from inside callbacks */
        return 0;
    }
    loop->running = 1;

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
                                        csilk_client_t* cli =
                                            (csilk_client_t*)((csilk_io_stream_t*)s)->data;
                                        pool_put_read_buf(
                                            cli ? cli->owner_pool : NULL, buf.base, buf.len);
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
                    /* Re-arm async poll */
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
                csilk_uv_on_write_done(ptr, res, gen);
            } else if (op == URING_OP_TMR_GENERIC) {
                csilk_io_timer_t* tmr = (csilk_io_timer_t*)ptr;
                /* io_uring timeout CQEs complete with -ETIME when the timer
                 * fires, and may return -ECANCELED when canceled concurrently;
                 * treat both the same as a successful completion. */
                if (tmr && (res >= 0 || res == -ECANCELED || res == -ETIME) &&
                    tmr->generation == gen && (tmr->flags & CSILK_IO_HANDLE_ACTIVE) &&
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

    loop->running = 0;
    return total > 0 ? 0 : -1;
}

#endif
