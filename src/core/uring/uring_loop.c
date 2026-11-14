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

_Thread_local csilk_io_loop_t  g_default_loop;
_Thread_local int              g_default_pending = 0;
_Thread_local int              g_default_loop_inited = 0;
_Thread_local struct io_uring* g_default_ring_ptr = NULL;

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
    _uring_deferred_drain_all();
    struct io_uring_cqe* cqe = NULL;
    while (io_uring_peek_cqe(&loop->ring, &cqe) == 0 && cqe) {
        uring_op_context_t* op_ctx = (uring_op_context_t*)(uintptr_t)cqe->user_data;
        if (op_ctx && op_ctx->slot_idx == UINT32_MAX) {
            free(op_ctx);
        }
        io_uring_cqe_seen(&loop->ring, cqe);
        cqe = NULL;
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
