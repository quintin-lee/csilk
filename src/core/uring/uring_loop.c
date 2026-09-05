/**
 * @file uring_loop.c
 * @brief io_uring event loop lifecycle: init, close, stop, now, default_loop.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <string.h>
#include "uring_internal.h"

/* ====================================================================
 * Thread-local default loop state
 * ==================================================================== */

csilk_io_loop_t  g_default_loop;
int              g_default_pending = 0;
int              g_default_loop_inited = 0;
struct io_uring* g_default_ring_ptr = NULL;

/* ====================================================================
 * Loop lifecycle
 * ==================================================================== */

int
csilk_io_loop_init(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    memset(loop, 0, sizeof(*loop));

    int entries = 1024;
    int rc = -1;
    while (entries >= 64) {
        rc = io_uring_queue_init((unsigned)entries, &loop->ring, 0);
        if (rc == 0) {
            break;
        }
        entries /= 2;
    }
    if (rc < 0) {
        return rc;
    }

    loop->op_pool_capacity = (uint32_t)(entries * 2);
    loop->op_pool = calloc(loop->op_pool_capacity, sizeof(uring_op_context_t));
    loop->op_free_stack = malloc(loop->op_pool_capacity * sizeof(uint32_t));
    if (loop->op_pool && loop->op_free_stack) {
        for (uint32_t i = 0; i < loop->op_pool_capacity; i++) {
            loop->op_free_stack[i] = loop->op_pool_capacity - 1 - i;
            loop->op_pool[i].slot_idx = i;
        }
        loop->op_free_head = loop->op_pool_capacity;
    }
    return 0;
}

int
csilk_io_loop_close(csilk_io_loop_t* loop)
{
    if (!loop) {
        return -1;
    }
    /* Cancel every pending operation on this ring, then reap completions
     * WITHOUT dispatching handlers (handles may already be freed by the
     * teardown that called us).  Reaping here releases each operation's
     * op-context; skipping the reap would leak one context per op still
     * in flight when the ring is destroyed. */
    struct io_uring*     ring = &loop->ring;
    struct io_uring_sqe* csqe = io_uring_get_sqe(ring);
    int                  cancel_armed = 0;
    if (csqe) {
        io_uring_prep_cancel64(csqe, 0, IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data64(csqe, 0);
        cancel_armed = io_uring_submit(ring) >= 1;
    }
    /* Block until the cancel-all CQE lands — the kernel posts it after the
     * CQEs of every cancelled operation, so once it is visible the ring is
     * fully drained.  The previous non-blocking peek broke out before the
     * cancelled timers' -ECANCELED/-ETIME completions arrived, leaking one
     * op-context per op still in flight at teardown (~4.7K in the e2e
     * keep-alive benchmark). */
    struct io_uring_cqe* wait_cqe = NULL;
    if (cancel_armed) {
        io_uring_wait_cqe_nr(ring, &wait_cqe, 1);
    }
    for (int spin = 0; spin < 10000; spin++) {
        struct io_uring_cqe* cqe = NULL;
        unsigned             head = 0;
        unsigned             reaped = 0;
        io_uring_for_each_cqe(ring, head, cqe)
        {
            reaped++;
            uring_op_context_t* op_ctx = (uring_op_context_t*)(uintptr_t)cqe->user_data;
            if (op_ctx) {
                uring_op_free(loop, op_ctx);
            }
        }
        if (reaped == 0) {
            break;
        }
        io_uring_cq_advance(ring, reaped);
    }
    /* Any overflow op-context that never completed (e.g. armed timers the
     * kernel never cancelled) is freed here; pool slots are covered by the
     * op_pool free below. */
    while (loop->op_overflow_head) {
        uring_op_context_t* next = loop->op_overflow_head->ovf_next;
        free(loop->op_overflow_head);
        loop->op_overflow_head = next;
    }
    io_uring_queue_exit(&loop->ring);
    if (loop->op_pool) {
        free(loop->op_pool);
        loop->op_pool = NULL;
    }
    if (loop->op_free_stack) {
        free(loop->op_free_stack);
        loop->op_free_stack = NULL;
    }
    if (loop == &g_default_loop) {
        g_default_loop_inited = 0;
        g_default_ring_ptr = NULL;
    }
    return 0;
}

void
csilk_io_stop(csilk_io_loop_t* loop)
{
    if (loop) {
        loop->stop_flag = 1;
    }
}

uint64_t
csilk_io_now(const csilk_io_loop_t* loop)
{
    (void)loop;
    return csilk_io_hrtime() / 1000000ULL;
}

void
csilk_io_update_time(csilk_io_loop_t* loop)
{
    (void)loop;
}

csilk_io_loop_t*
csilk_io_default_loop(void)
{
    if (!g_default_loop_inited) {
        g_default_loop_inited = 1;
        if (csilk_io_loop_init(&g_default_loop) != 0) {
            return NULL;
        }
        g_default_ring_ptr = &g_default_loop.ring;
    }
    return &g_default_loop;
}

#endif
