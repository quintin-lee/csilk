/**
 * @file uring_run.c
 * @brief io_uring event loop dispatch: csilk_io_run and write_done callback.
 *
 * Contains the CQE polling loop with handlers for all operation types
 * (listen, read, async, signal, write, timer).
 *
 * Completion lifecycle:
 *   Each CQE carries an opaque ptr + generation tag from its SQE.
 *   On completion we verify (a) the handle is still alive (ACTIVE && !CLOSING)
 *   and (b) the generation matches — if either fails the CQE is stale and
 *   silently dropped.  Timers accept both -ETIME (fired) and -ECANCELED
 *   (cancel raced with fire) as successful completions.
 *
 * @copyright MIT License
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
 * Helpers
 * ==================================================================== */

/** @brief Check whether a CQE is stale for a poll-based handle.
 *
 *  A CQE is stale when the handle has been closed or its generation
 *  was bumped (handle reused).  Returns 1 if the completion should be
 *  dropped, 0 if it is still valid. */
static int
is_stale_poll(csilk_io_handle_t* handle, uint64_t gen)
{
    if (!handle || handle->fd < 0) {
        return 1;
    }
    if ((handle->flags & CSILK_IO_HANDLE_ACTIVE) == 0) {
        return 1;
    }
    if (handle->flags & CSILK_IO_HANDLE_CLOSING) {
        return 1;
    }
    if (handle->generation != gen) {
        return 1;
    }
    return 0;
}

/** @brief Check whether a CQE is stale for a timer handle.
 *
 *  Timer completions are considered stale when the handle has been
 *  closed, is closing, or its generation no longer matches the one
 *  encoded in the CQE user_data. */
static int
is_stale_timer(csilk_io_timer_t* tmr, uint64_t gen)
{
    if (!tmr) {
        return 1;
    }
    if ((tmr->flags & CSILK_IO_HANDLE_ACTIVE) == 0) {
        return 1;
    }
    if (tmr->flags & CSILK_IO_HANDLE_CLOSING) {
        return 1;
    }
    if (tmr->generation != gen) {
        return 1;
    }
    return 0;
}

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

            int                 res = cqe->res;
            uint64_t            gen = 0;
            void*               ptr = NULL;
            uring_op_type_t     op = URING_OP_NONE;
            uring_op_context_t* op_ctx = (uring_op_context_t*)(uintptr_t)cqe->user_data;

            if (op_ctx) {
                op = (uring_op_type_t)op_ctx->type;
                ptr = op_ctx->data ? op_ctx->data : op_ctx->owner;
                gen = op_ctx->generation;
                uring_op_free(loop, op_ctx);
            }

            /* ----------------------------------------------------------------
             * URING_OP_POLL_LISTEN
             * Re-arm the listen poll and call the connection callback.
             * Stale completions are dropped when the handle is no longer
             * active or is being closed.
             * ---------------------------------------------------------------- */
            if (op == URING_OP_POLL_LISTEN) {
                csilk_io_stream_t* s = (csilk_io_stream_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)s, gen)) {
                    continue;
                }
                /* Re-arm listen poll */
                struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, s->fd, POLLIN);
                    io_uring_sqe_set_data64(
                        sqe, uring_encode_handle_data(URING_OP_POLL_LISTEN, (csilk_io_handle_t*)s));
                    io_uring_submit(ring);
                }
                if (res >= 0 && s->connection_cb) {
                    s->connection_cb(s, 0);
                }

                /* ----------------------------------------------------------------
                 * URING_OP_READ / URING_OP_POLL_READ
                 * Native IORING_OP_RECV completion path (zero-syscall read path).
                 * Stale completions are dropped when generation mismatches or
                 * the handle is closing.
                 * Connection errors (ECONNRESET, EPIPE) are translated to EOF.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_READ || op == URING_OP_POLL_READ) {
                csilk_io_stream_t* s = (csilk_io_stream_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)s, gen)) {
                    if (s && s->recv_buf.base) {
                        pool_put_read_buf(NULL, s->recv_buf.base, s->recv_buf.len);
                        s->recv_buf.base = NULL;
                        s->recv_buf.len = 0;
                    }
                    continue;
                }
                if (res > 0) {
                    /* Data was received directly into s->recv_buf by kernel */
                    csilk_io_buf_t buf = s->recv_buf;
                    s->recv_buf.base = NULL;
                    s->recv_buf.len = 0;

                    if (s->read_cb) {
                        s->read_cb(s, (ssize_t)res, &buf);
                    }

                    /* Re-arm native IORING_OP_RECV if still reading */
                    if (s->reading && s->fd >= 0 && (s->flags & CSILK_IO_HANDLE_ACTIVE) &&
                        !(s->flags & CSILK_IO_HANDLE_CLOSING)) {
                        if (s->alloc_cb) {
                            s->alloc_cb((csilk_io_handle_t*)s, 65536, &s->recv_buf);
                        }
                        if (s->recv_buf.base && s->recv_buf.len > 0) {
                            struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                            if (sqe) {
                                io_uring_prep_recv(
                                    sqe, s->fd, s->recv_buf.base, s->recv_buf.len, 0);
                                io_uring_sqe_set_data64(
                                    sqe,
                                    uring_encode_handle_data(URING_OP_READ, (csilk_io_handle_t*)s));
                                io_uring_submit(ring);
                            }
                        }
                    }
                } else if (res == 0) {
                    /* EOF: peer closed the connection */
                    csilk_io_buf_t buf = s->recv_buf;
                    s->recv_buf.base = NULL;
                    s->recv_buf.len = 0;

                    s->reading = 0;
                    s->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                    if (loop->active_handles > 0) {
                        loop->active_handles--;
                    }
                    if (s->read_cb) {
                        s->read_cb(s, -4095 /* UV_EOF */, &buf);
                    }
                } else {
                    /* res < 0: Error */
                    if (res == -ECANCELED) {
                        /* In-flight recv cancelled during read_stop/close */
                        if (s->recv_buf.base) {
                            pool_put_read_buf(NULL, s->recv_buf.base, s->recv_buf.len);
                            s->recv_buf.base = NULL;
                            s->recv_buf.len = 0;
                        }
                    } else if (res == -EAGAIN || res == -EWOULDBLOCK) {
                        /* Transient — re-arm recv SQE with current buffer */
                        if (s->reading && s->fd >= 0 && (s->flags & CSILK_IO_HANDLE_ACTIVE) &&
                            !(s->flags & CSILK_IO_HANDLE_CLOSING)) {
                            struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                            if (sqe) {
                                io_uring_prep_recv(
                                    sqe, s->fd, s->recv_buf.base, s->recv_buf.len, 0);
                                io_uring_sqe_set_data64(
                                    sqe,
                                    uring_encode_handle_data(URING_OP_READ, (csilk_io_handle_t*)s));
                                io_uring_submit(ring);
                            }
                        }
                    } else {
                        /* Fatal socket error — translate ECONNRESET/EPIPE/ETIMEDOUT to EOF */
                        int err = -res;
                        if (err == ECONNRESET || err == EPIPE || err == ETIMEDOUT) {
                            err = 0; /* EOF */
                        }
                        csilk_io_buf_t buf = s->recv_buf;
                        s->recv_buf.base = NULL;
                        s->recv_buf.len = 0;

                        s->reading = 0;
                        s->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                        if (loop->active_handles > 0) {
                            loop->active_handles--;
                        }
                        if (s->read_cb) {
                            if (err == 0) {
                                s->read_cb(s, -4095 /* UV_EOF */, &buf);
                            } else {
                                s->read_cb(s, -err, &buf);
                            }
                        }
                    }
                }

                /* ----------------------------------------------------------------
                 * URING_OP_POLL_ASYNC
                 * Drain the eventfd and re-arm.  Stale if handle closed/reused.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_POLL_ASYNC) {
                csilk_io_async_t* async = (csilk_io_async_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)async, gen)) {
                    continue;
                }
                uint64_t val = 0;
                ssize_t  nr = read(async->event_fd, &val, sizeof(val));
                /* Re-arm async poll */
                struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, async->event_fd, POLLIN);
                    io_uring_sqe_set_data64(
                        sqe,
                        uring_encode_handle_data(URING_OP_POLL_ASYNC, (csilk_io_handle_t*)async));
                    io_uring_submit(ring);
                }
                if (nr > 0 && async->cb) {
                    async->cb(async);
                }

                /* ----------------------------------------------------------------
                 * URING_OP_POLL_SIGNAL
                 * Drain the signalfd and re-arm.  Stale if handle closed/reused.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_POLL_SIGNAL) {
                csilk_io_signal_t* sig = (csilk_io_signal_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)sig, gen)) {
                    continue;
                }
                struct signalfd_siginfo fdsi;
                ssize_t                 nr = read(sig->signal_fd, &fdsi, sizeof(fdsi));
                /* Re-arm signal poll */
                struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, sig->signal_fd, POLLIN);
                    io_uring_sqe_set_data64(
                        sqe,
                        uring_encode_handle_data(URING_OP_POLL_SIGNAL, (csilk_io_handle_t*)sig));
                    io_uring_submit(ring);
                }
                if (nr > 0 && sig->cb) {
                    sig->cb(sig, sig->signum);
                }

                /* ----------------------------------------------------------------
                 * URING_OP_UV_WRITE
                 * Defer to csilk_uv_on_write_done which checks client generation.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_UV_WRITE) {
                csilk_uv_on_write_done(ptr, res, gen);

                /* ----------------------------------------------------------------
                 * URING_OP_TMR_GENERIC
                 * Timer fired or cancelled.  Both -ETIME (fired) and
                 * -ECANCELED (cancel raced with fire) are treated as success.
                 * Generation mismatch or CLOSING state means stale — drop.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_TMR_GENERIC) {
                csilk_io_timer_t* tmr = (csilk_io_timer_t*)ptr;
                if (is_stale_timer(tmr, gen)) {
                    continue;
                }
                /* io_uring timeout CQEs: -ETIME = timer fired, -ECANCELED =
                 * cancel raced with fire.  Both are accepted as successful. */
                if (res >= 0 || res == -ECANCELED || res == -ETIME) {
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
                        /* One-shot timer: deactivate and drop active count. */
                        tmr->flags &= ~CSILK_IO_HANDLE_ACTIVE;
                        if (tmr->loop && tmr->loop->active_handles > 0) {
                            tmr->loop->active_handles--;
                        }
                    }
                    if (cb) {
                        cb(tmr);
                    }
                }
                /* else: genuine error (e.g. kernel reject) — drop silently. */
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
