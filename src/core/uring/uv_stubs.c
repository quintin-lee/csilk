/**
 * @file uv_stubs.c
 * @brief libuv-compatible API shims for the io_uring backend.
 *
 * Implements the subset of the csilk I/O surface that the io_uring backend must
 * expose to look like the libuv backend: handle closing flags, stream fileno,
 * vectored writes, and timer lifecycle/default-loop/run glue. Most routines are
 * thin wrappers that translate libuv-style requests into io_uring SQEs, with a
 * few no-op callbacks used by the synchronous fallback path.
 *
 * @copyright MIT License
 */

#include <csilk/core/sys_io.h>

#ifdef CSILK_USE_URING

#include <csilk/csilk.h>
#include <csilk/core/server.h>
#include <csilk/core/internal.h>
#include "../internal/srv_internal.h"
#include "uring_internal.h"

void csilk_client_close(csilk_client_t* client);

/**
 * @brief Report whether an I/O handle is in a closing state.
 * @param[in] handle The handle to query (unused by the io_uring backend).
 * @return Always 0; the io_uring backend has no libuv closing state machine.
 */
int
csilk_io_is_closing(const csilk_io_handle_t* handle)
{
    (void)handle;
    /* io_uring backend has no libuv closing state machine.
     * Returns 0 (not closing) for all handles. */
    return 0;
}

/**
 * @brief Retrieve the OS file descriptor backing an I/O handle.
 * @param[in]  handle Stream handle whose fd is requested.
 * @param[out] fd     Receives the OS file descriptor on success.
 * @return 0 on success, -1 on NULL handle/fd.
 * @note The handle is downcast to csilk_io_stream_t; only stream handles carry
 *       an fd.
 */
int
csilk_io_fileno(const csilk_io_handle_t* handle, csilk_io_os_fd_t* fd)
{
    if (!handle || !fd) {
        return -1;
    }
    csilk_io_stream_t* stream = (csilk_io_stream_t*)handle;
    *fd = stream->fd;
    return 0;
}

/**
 * @brief Submit a vectored write for a stream over io_uring.
 * @param[in] req     Write request object; its cb/handle fields are filled in.
 * @param[in] handle  Stream handle (its data must point at a csilk_client_t).
 * @param[in] bufs    Array of iovec-style buffers to write.
 * @param[in] nbufs   Number of buffers in bufs.
 * @param[in] cb      Completion callback invoked from the CQE dispatcher.
 * @return 0 on successful submission, -1 on NULL/invalid handle, closed
 *         connection, or when no SQE slot is available.
 * @note Allocates an iovec array and a 3-pointer context that travel with the
 *       SQE and are freed in csilk_uv_on_write_done. Increments the client's
 *       async_ref so the connection is not torn down mid-write.
 */
int
csilk_io_write(csilk_io_write_t*    req,
               csilk_io_stream_t*   handle,
               const csilk_io_buf_t bufs[],
               unsigned int         nbufs,
               csilk_io_write_cb    cb)
{
    if (!handle || !handle->data) {
        return -1;
    }
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client->ctx.conn_closed) {
        return -1;
    }

    struct io_uring*     ring = client->owner_pool->loop_ptr;
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
    if (!sqe) {
        return -1;
    }

    req->cb = (void*)cb;
    req->handle = handle;

    struct iovec* iov = malloc(sizeof(struct iovec) * nbufs);
    for (unsigned int i = 0; i < nbufs; ++i) {
        iov[i].iov_base = bufs[i].base;
        iov[i].iov_len = bufs[i].len;
    }

    io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);

    void** ctx = malloc(sizeof(void*) * 3);
    ctx[0] = client;
    ctx[1] = req;
    ctx[2] = iov;
    io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_UV_WRITE, client, ctx));
    atomic_fetch_add(&client->async_ref, 1);
    io_uring_submit(ring);
    return 0;
}

/**
 * @brief Close an I/O handle on the io_uring backend.
 * @param[in] handle Stream/timer/async handle to close.
 * @param[in] cb     Optional close callback (invoked for stream handles only).
 * @note For stream handles whose data points at a client, delegates to
 *       csilk_client_close. Non-stream handles (timer/async) have no libuv
 *       close semantics, so the callback is a no-op for them. NULL handles are
 *       ignored.
 */
void
csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb)
{
    if (!handle) {
        return;
    }
    /* In the uring backend, only TCP handles (csilk_io_tcp_t) wrap clients.
     * Distinguish by checking if data points to a valid client structure
     * (has ctx.conn_closed at a known offset). Async/timer handles have
     * different layouts and must not be cast to stream. */
    if (handle->data && ((uintptr_t)handle->data > 0x1000)) {
        csilk_io_stream_t* stream = (csilk_io_stream_t*)handle;
        csilk_client_t*    client = (csilk_client_t*)stream->data;
        /* Verify this looks like a real client (not a random pointer) */
        if (client && client->server && client->handle.fd >= 0) {
            csilk_client_close(client);
        }
    }
    /* For non-stream handles (async, timer, etc.) the callback is a no-op
     * since there's no libuv close semantics in the io_uring backend. */
    if (cb && handle->data && ((uintptr_t)handle->data > 0x1000)) {
        cb(handle);
    }
}

/**
 * @brief No-op libuv close callback for the io_uring backend.
 * @param[in] handle Unused.
 */
void
on_close(csilk_io_handle_t* handle)
{
}
/**
 * @brief No-op idle timeout callback (sync fallback path).
 * @param[in] handle Unused.
 */
void
on_idle_timeout(csilk_io_timer_t* handle)
{
}
/**
 * @brief No-op read timeout callback (sync fallback path).
 * @param[in] handle Unused.
 */
void
on_read_timeout(csilk_io_timer_t* handle)
{
}
/**
 * @brief No-op write timeout callback (sync fallback path).
 * @param[in] handle Unused.
 */
void
on_write_timeout(csilk_io_timer_t* handle)
{
}

/* --- Default ring + pending-SQE tracker --- */

/* Lazy-init singleton io_uring ring used by csilk_io_default_loop().
 *
 * This ring is the DEFAULT loop — it is distinct from the per-worker
 * rings the server creates inside uring_thread().  Its main purpose in
 * test / CLI programs is to give csilk_io_timer_start() a valid ring so
 * timeout SQEs can be submitted.  The ring is NOT thread-safe; callers
 * must ensure no concurrent sqe/cqe access (the default is single-
 * threaded test or CLI use). */
static struct io_uring g_default_ring;
static int             g_default_ring_inited = 0;

/* Counter: how many SQEs have been submitted to the default ring (via
 * csilk_io_timer_start/stop) but whose completion CQEs have not yet
 * been consumed by csilk_io_run().  Accessed from test/CLI code only
 * (single thread), so no locking is required. */
static int g_default_pending = 0;

/* Pointer to the default ring so timer functions can check whether
 * they are submitting to the default ring (vs a per-worker ring). */
static struct io_uring* g_default_ring_ptr = NULL;

/**
 * @brief Initialize a timer handle and bind it to its owning loop.
 * @param[in]  loop   Event loop that owns the timer (stored as ring).
 * @param[out] handle Timer handle to initialize; fd set to -1, generation to 1.
 * @return Always 0.
 */
int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
    handle->fd = -1;
    handle->data = NULL;
    handle->ring = loop;
    handle->cb = NULL;
    handle->generation = 1; /* Start at 1 so the first start() is a new generation. */
    return 0;
}

/**
 * @brief Arm a timer on the io_uring backend.
 * @param[in] handle  Timer handle (must already have a valid ring).
 * @param[in] cb      Callback invoked on expiry.
 * @param[in] timeout Expiry in milliseconds.
 * @param[in] repeat  Repeat interval in milliseconds (0 for one-shot).
 * @return 0 on success, -1 on NULL handle/ring or when no SQE slot is free.
 * @note Bumps the handle generation so stale CQEs from a previous interval are
 *       ignored. Submits an io_uring timeout SQE tagged URING_OP_TMR_GENERIC.
 */
int
csilk_io_timer_start(csilk_io_timer_t* handle,
                     csilk_io_timer_cb cb,
                     uint64_t          timeout,
                     uint64_t          repeat)
{
    if (!handle || !handle->ring) {
        return -1;
    }

    /* Bump generation so stale CQEs from a previous interval are ignored. */
    handle->generation++;

    struct io_uring*         ring = handle->ring;
    struct __kernel_timespec ts;
    ts.tv_sec = (__kernel_time64_t)(timeout / 1000);
    ts.tv_nsec = (long long)(timeout % 1000) * 1000000LL;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
    if (!sqe) {
        return -1;
    }

    handle->cb = cb;
    io_uring_prep_timeout(sqe, &ts, repeat ? 1 : 0, 0);
    io_uring_sqe_set_data64(sqe, uring_encode_timer_data(URING_OP_TMR_GENERIC, handle));
    io_uring_submit(ring);
    if (ring == g_default_ring_ptr) {
        g_default_pending++;
    }
    return 0;
}

/**
 * @brief Disarm a timer on the io_uring backend.
 * @param[in] handle Timer handle to stop.
 * @return 0 on success, -1 on NULL handle/ring or when no SQE slot is free.
 * @note Bumps the generation and submits an io_uring cancel SQE targeting the
 *       pending timeout; its completion is ignored via generation mismatch.
 */
int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
    if (!handle || !handle->ring) {
        return -1;
    }

    handle->cb = NULL;
    handle->generation++;

    /* Send a cancel request for the pending timeout.  The cancel completion
     * will arrive as a TMR_GENERIC CQE with the old generation, so the
     * handler will skip it (cb == NULL, or generation mismatch). */
    struct io_uring*     ring = handle->ring;
    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(ring);
    if (!sqe) {
        return -1;
    }

    uint64_t cancel_val = uring_encode_timer_data(URING_OP_TMR_GENERIC, handle);
    io_uring_prep_cancel(sqe, (void*)cancel_val, 0);
    io_uring_sqe_set_data64(sqe, cancel_val);
    io_uring_submit(ring);
    /* Don't track pending for cancel ops — the CQE will be skipped
     * (generation mismatch), so we'd never decrement g_default_pending. */
    return 0;
}

/**
 * @brief Return (lazily initializing) the process-wide default io_uring loop.
 * @return Pointer to the default loop, or NULL if ring initialization failed.
 * @note The default ring is single-threaded (test/CLI use) and is distinct from
 *       the per-worker rings the server creates. Initialized exactly once.
 */
csilk_io_loop_t*
csilk_io_default_loop(void)
{
    if (!g_default_ring_inited) {
        g_default_ring_inited = 1;
        if (io_uring_queue_init(1024, &g_default_ring, 0) != 0) {
            return NULL;
        }
        g_default_ring_ptr = &g_default_ring;
    }
    return (csilk_io_loop_t*)&g_default_ring;
}

/**
 * @brief Run an io_uring loop, draining deferred work and timer CQEs.
 * @param[in] loop Event loop to run; falls back to the default loop if NULL.
 * @param[in] mode Run mode: NOWAIT (single poll), ONCE (one poll + optional
 *                 short wait), or DEFAULT (run until no pending work remains).
 * @return 0 if any work was processed, -1 if the loop was NULL/unavailable or
 *         no work was processed.
 * @note Iteratively drains deferred after-work callbacks and processes
 *       URING_OP_TMR_GENERIC timer completions, skipping stale generations.
 */
int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return -1;
        }
    }

    struct io_uring* ring = (struct io_uring*)loop;
    int              total = 0;

    do {
        /* 1. Drain deferred after-work callbacks (iteratively). */
        int n;
        while ((n = _uring_deferred_drain_all()) > 0) {
            total += n;
        }

        /* 2. Process io_uring CQEs on the ring.  In test/CLI mode
         *    these are TMR_GENERIC timer completions. */
        struct io_uring_cqe* cqe;
        unsigned             head;
        unsigned             cq_count = 0;
        io_uring_for_each_cqe(ring, head, cqe)
        {
            cq_count++;
            uint8_t         gen;
            void*           ptr;
            uring_op_type_t op;
            uring_decode_data(cqe->user_data, &op, &ptr, &gen);
            if (op == URING_OP_TMR_GENERIC) {
                csilk_io_timer_t* tmr = (csilk_io_timer_t*)ptr;
                if (tmr && tmr->generation == gen && tmr->cb) {
                    tmr->cb(tmr);
                }
            }
        }
        if (cq_count > 0) {
            io_uring_cq_advance(ring, cq_count);
            /* Each CQE consumed means one SQE submission completed. */
            if (ring == g_default_ring_ptr) {
                g_default_pending -= (int)cq_count;
                if (g_default_pending < 0) {
                    g_default_pending = 0;
                }
            }

            /* Timer callbacks may queue deferred work — drain again. */
            while ((n = _uring_deferred_drain_all()) > 0) {
                total += n;
            }
        }

        /* 3. Mode-specific decision */
        if (mode == CSILK_IO_RUN_NOWAIT) {
            break;
        }

        if (mode == CSILK_IO_RUN_ONCE) {
            if (cq_count == 0) {
                /* Wait briefly (10 ms) for a timer completion. */
                struct __kernel_timespec ts = {0, 10000000};
                io_uring_wait_cqe_timeout(ring, &cqe, &ts);
            }
            break;
        }

        /* CSILK_IO_RUN_DEFAULT */
        if (cq_count == 0) {
            if (ring == g_default_ring_ptr && g_default_pending > 0) {
                /* There are SQEs in-flight — wait for their CQEs.
                 * Use a short timeout so we re-check the deferred queue
                 * and the counter periodically. */
                struct __kernel_timespec ts = {0, 10000000}; /* 10 ms */
                io_uring_wait_cqe_timeout(ring, &cqe, &ts);
            } else {
                /* No pending work on this ring — we are done. */
                break;
            }
        }
    } while (1);

    return total > 0 ? 0 : -1;
}

#endif

/**
 * @brief Completion handler for a uving-style vectored write SQE.
 * @param[in] arg Context pointer (client, write req, iovec triple) allocated by
 *                csilk_io_write.
 * @param[in] res Bytes written, or a negative error.
 * @note Frees the iovec and context, invokes the user write callback, then
 *       releases the client's async_ref; if a close is pending and the ref
 *       hits zero it triggers on_close_done. No-op if arg is NULL.
 */
void
csilk_uv_on_write_done(void* arg, ssize_t res)
{
    void** ctx = (void**)arg;
    if (!ctx) {
        return;
    }
    csilk_client_t*   client = (csilk_client_t*)ctx[0];
    csilk_io_write_t* req = (csilk_io_write_t*)ctx[1];
    struct iovec*     iov = (struct iovec*)ctx[2];

    if (iov) {
        free(iov);
    }
    if (req && req->cb) {
        csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
        cb(req, (int)res);
    }
    free(ctx);

    atomic_fetch_sub(&client->async_ref, 1);
    if (client->close_pending && atomic_load(&client->async_ref) == 0) {
        on_close_done(client);
    }
}
