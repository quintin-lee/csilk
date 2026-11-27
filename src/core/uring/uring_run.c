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
#include <string.h>
#include <sys/signalfd.h>

#include "../internal/srv_internal.h"
#include "uring_internal.h"

/* Forward declarations for functions defined in server modules */
extern void on_read(csilk_client_t* client, ssize_t nread);
extern void on_timeout(csilk_client_t* client);
extern void client_destroy(csilk_client_t* client);
extern void csilk_client_close(csilk_client_t* client);
extern void on_close_done(csilk_client_t* client);

extern void           pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);
extern worker_pool_t* _csilk_worker_get_current_pool(void);

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
                if (op_ctx->flags & URING_OP_CTX_CANCELLED) {
                    uring_op_free(loop, op_ctx);
                    continue;
                }
                op = (uring_op_type_t)op_ctx->type;
                ptr = op_ctx->data ? op_ctx->data : op_ctx->owner;
                gen = op_ctx->generation;
                if (op == URING_OP_TMR_GENERIC && ptr) {
                    csilk_io_timer_t* tmr_ptr = (csilk_io_timer_t*)ptr;
                    if (tmr_ptr->op == (csilk_io_timer_op_t*)op_ctx) {
                        tmr_ptr->op = NULL;
                    }
                }
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
                if (res >= 0 && s->connection_cb) {
                    s->connection_cb(s, 0);
                }
                /* Re-arm listen poll only after connection_cb drains the accept
                 * queue and only if the listener was not closed in the callback. */
                if (!is_stale_poll((csilk_io_handle_t*)s, gen) && s->fd >= 0) {
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, s->fd, POLLIN);
                        io_uring_sqe_set_data64(
                            sqe,
                            uring_encode_handle_data(URING_OP_POLL_LISTEN, (csilk_io_handle_t*)s));
                        io_uring_submit(ring);
                    }
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
                    if (s && s->recv_buf.base && !s->reading) {
                        pool_put_read_buf(
                            _csilk_worker_get_current_pool(), s->recv_buf.base, s->recv_buf.len);
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
                        if (s->recv_buf.base && !s->reading) {
                            pool_put_read_buf(_csilk_worker_get_current_pool(),
                                              s->recv_buf.base,
                                              s->recv_buf.len);
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
                int event_fd_val = atomic_load_explicit(&async->event_fd, memory_order_relaxed);
                uint64_t val = 0;
                ssize_t  nr = read(event_fd_val, &val, sizeof(val));
                /* Re-arm async poll */
                struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                if (sqe) {
                    io_uring_prep_poll_add(sqe, event_fd_val, POLLIN);
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
                 * URING_OP_POLL
                 * General file-descriptor / socket poll event.
                 * ---------------------------------------------------------------- */
            } else if (op == URING_OP_POLL) {
                csilk_io_poll_t* poll_h = (csilk_io_poll_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)poll_h, gen)) {
                    continue;
                }
                int cqe_res = cqe->res;
                int events = 0;
                if (cqe_res > 0) {
                    if (cqe_res & (POLLIN | POLLPRI)) {
                        events |= CSILK_IO_READABLE;
                    }
                    if (cqe_res & POLLOUT) {
                        events |= CSILK_IO_WRITABLE;
                    }
                    if (cqe_res & (POLLHUP | POLLERR)) {
                        events |= CSILK_IO_DISCONNECT;
                    }
                }
                /* Re-arm if still active */
                if ((poll_h->flags & CSILK_IO_HANDLE_ACTIVE) &&
                    !(poll_h->flags & CSILK_IO_HANDLE_CLOSING)) {
                    short poll_mask = 0;
                    if (poll_h->events & CSILK_IO_READABLE) {
                        poll_mask |= POLLIN | POLLPRI;
                    }
                    if (poll_h->events & CSILK_IO_WRITABLE) {
                        poll_mask |= POLLOUT;
                    }
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        io_uring_prep_poll_add(sqe, poll_h->fd, poll_mask);
                        io_uring_sqe_set_data64(
                            sqe,
                            uring_encode_handle_data(URING_OP_POLL, (csilk_io_handle_t*)poll_h));
                        io_uring_submit(ring);
                    }
                }
                if (poll_h->cb) {
                    int status = cqe_res >= 0 ? 0 : cqe_res;
                    poll_h->cb(poll_h, status, events);
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
                        uint64_t next_data = uring_encode_timer_data(URING_OP_TMR_GENERIC, tmr);
                        if (next_data) {
                            struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                            if (sqe) {
                                tmr->op = (csilk_io_timer_op_t*)(uintptr_t)next_data;
                                io_uring_prep_timeout(sqe, &ts, 0, 0);
                                io_uring_sqe_set_data64(sqe, next_data);
                                io_uring_submit(ring);
                            } else {
                                /* Re-arm failed (SQE pool full / submit error):
                                 * the repeating timer will not fire again. Log
                                 * so the silent stall is diagnosable, then free
                                 * the op context so it is not leaked. */
                                CSILK_LOG_W("uring: failed to re-arm repeating timer %p; timer "
                                            "will stop firing",
                                            (void*)tmr);
                                uring_op_free(loop, (uring_op_context_t*)(uintptr_t)next_data);
                            }
                        } else {
                            /* uring_encode_timer_data returned 0 (op pool
                             * exhausted): the repeating timer cannot be
                             * re-armed and will stop firing. */
                            CSILK_LOG_W(
                                "uring: op pool exhausted; repeating timer %p will stop firing",
                                (void*)tmr);
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
            } else if (op == URING_OP_UDP_RECV) {
                /*
                 * IORING_OP_RECVMSG multishot completion.
                 *
                 * The kernel delivers one CQE per datagram, writing:
                 *   – the io_uring_recvmsg_out header + sender address +
                 *     payload directly into the registered buffer-ring slot
                 *   – cqe->res = payload bytes (or -errno on error)
                 *   – cqe->flags = IORING_CQE_F_MORE  while the multishot
                 *     stream is still active; the final CQE (stream ended,
                 *     cancelled, or an error that stopped it) has MORE cleared.
                 *
                 * We read the datagram from the ring slot, copy the payload
                 * into handle->recv_buf (user-facing buffer), recycle the
                 * slot, and call the user callback.
                 *
                 * Re-arm policy: when the kernel stops the stream
                 * (!IORING_CQE_F_MORE) we must submit a fresh multishot SQE
                 * so subsequent datagrams keep flowing.  This mirrors the
                 * pattern in liburing's io_uring-udp example.
                 */
                csilk_io_udp_t* udp = (csilk_io_udp_t*)ptr;
                if (is_stale_poll((csilk_io_handle_t*)udp, gen)) {
                    continue;
                }

                /*
                 * Successful datagram delivery.
                 * cqe->res > 0 and IORING_CQE_F_BUFFER set → the payload
                 * lives in the selected ring slot.
                 */
                if (res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    unsigned bid = cqe->flags >> 16;
                    if (bid >= (unsigned)loop->udp_nbufs || !loop->udp_storage ||
                        !loop->udp_buf_ring) {
                        continue;
                    }
                    /*
                     * The ring slot was allocated by csilk_io_loop_setup_udp_buf_ring
                     * as a single contiguous block.  Each slot is loop->udp_bufsz
                     * bytes; slot bid sits at udp_storage + bid * udp_bufsz.
                     */
                    void* slot_base =
                        (char*)loop->udp_storage + (size_t)bid * (size_t)loop->udp_bufsz;

                    struct io_uring_recvmsg_out* o =
                        io_uring_recvmsg_validate(slot_base, res, &udp->msg);
                    if (o) {
                        void*    payload = io_uring_recvmsg_payload(o, &udp->msg);
                        unsigned payload_len = io_uring_recvmsg_payload_length(o, res, &udp->msg);
                        if (payload && payload_len > sizeof(udp->recv_buf)) {
                            payload_len = sizeof(udp->recv_buf);
                        }
                        if (payload && payload_len > 0) {
                            memcpy(udp->recv_buf, payload, payload_len);
                        }

                        /* Cache sender address for subsequent sends (bounded copy). */
                        void*  sender = io_uring_recvmsg_name(o);
                        size_t sender_len = o->namelen;
                        if (sender && sender_len > 0) {
                            if (sender_len > sizeof(udp->remote_storage)) {
                                sender_len = sizeof(udp->remote_storage);
                            }
                            memcpy(&udp->remote_storage, sender, sender_len);
                            udp->remote_len = (int)sender_len;
                        }

                        if (udp->cb) {
                            csilk_io_buf_t csbuf;
                            csbuf.base = udp->recv_buf;
                            csbuf.len = payload_len;
                            const struct sockaddr* sender_addr = (const struct sockaddr*)sender;
                            udp->cb(udp, (ssize_t)payload_len, &csbuf, sender_addr);
                        }
                    }

                    /*
                     * Recycle the ring slot so the kernel can use it for the
                     * next datagram.  The slot address is the same as before
                     * (ring slots are fixed); only the bid tag changes.
                     */
                    io_uring_buf_ring_add(loop->udp_buf_ring,
                                          slot_base,
                                          (unsigned)loop->udp_bufsz,
                                          (unsigned short)bid,
                                          io_uring_buf_ring_mask(loop->udp_nbufs),
                                          0);
                    io_uring_buf_ring_advance(loop->udp_buf_ring, 1);
                } else if (res == -EAGAIN) {
                    /*
                     * Transient: the socket has no pending data at this instant.
                     * The kernel still holds the multishot SQE armed (MORE may
                     * or may not be set in this CQE — on some kernels an
                     * -EAGAIN CQE also has MORE cleared, which means we need
                     * to re-arm below; on others it has MORE set and the kernel
                     * will keep the SQE alive).  In either case we handle the
                     * re-arm in the !MORE branch below, so no extra work here.
                     */
                } else if (res < 0) {
                    /*
                     * Permanent error (-EINVAL, -EBADF, -EIO, …).
                     * The multishot stream is dead; log and stop the handle
                     * so the user can re-start it if appropriate.
                     */
                    CSILK_LOG_E(
                        "uring: UDP recvmsg multishot error on fd %d: %s", udp->fd, strerror(-res));
                    csilk_io_udp_recv_stop(udp);
                }

                /*
                 * Re-arm the multishot SQE when the kernel has stopped the
                 * stream.  This happens when the CQE has IORING_CQE_F_MORE
                 * cleared, which is the final CQE in a multishot batch.
                 * Without re-arming, no further datagrams will be delivered.
                 *
                 * We use udp->msg (handle-embedded, stable) not a stack
                 * msghdr, because the kernel reads the msghdr pointer at
                 * CQE completion time, not at SQE submission time.
                 */
                if (!(cqe->flags & IORING_CQE_F_MORE) && udp->recv_started && udp->fd >= 0) {
                    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
                    if (sqe) {
                        uint64_t next_data =
                            uring_encode_handle_data(URING_OP_UDP_RECV, (csilk_io_handle_t*)udp);
                        if (next_data) {
                            io_uring_prep_recvmsg_multishot(sqe, udp->fd, &udp->msg, 0);
                            sqe->buf_group = 0;
                            sqe->flags |= IOSQE_BUFFER_SELECT;
                            io_uring_sqe_set_data64(sqe, next_data);
                            io_uring_submit(ring);
                        } else {
                            /*
                             * SQE pool exhausted: the multishot stream will not
                             * re-arm.  Log so the silent stall is diagnosable.
                             */
                            CSILK_LOG_W("uring: UDP recvmsg re-arm failed for fd %d "
                                        "(SQE pool exhausted); stream stopped",
                                        udp->fd);
                        }
                    }
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

    /* Drain any after-work callbacks left on this worker's TLS deferred
     * queue. The run loop normally consumes these each iteration (see above),
     * but a NOWAIT/ONCE or stop_flag exit may leave a few pending; csilk_io
     * consumers that stop() the loop without going through csilk_io_loop_close
     * would otherwise leak them (their client refs / gzip state). */
    while (_uring_deferred_drain_all() > 0) {
    }

    loop->running = 0;
    return total > 0 ? 0 : -1;
}

#endif
