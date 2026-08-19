#ifndef CSILK_URING_INTERNAL_H
#define CSILK_URING_INTERNAL_H

/**
 * @file uring_internal.h
 * @brief Internal declarations and helpers for the io_uring event-loop backend.
 *
 * Defines the io_uring opcode set used to tag SQE user_data, the per-operation
 * payload structs, and a set of inline helpers that pack/unpack operation type,
 * client pointer, and generation counters into the 64-bit user_data word so the
 * CQE dispatcher can route completions without indirect calls. Also forward
 * declares the internal worker, thread-pool, barrier, and server-split routines
 * shared across the uring directory translation units.

 *
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"
#include "../internal/srv_internal.h" /* csilk_client_t full definition (for generation field access) */

/* Define opcodes for user_data.
 * High bit selects between I/O ops and timer ops so the CQE dispatch
 * can quickly distinguish completion types without an indirect call. */
typedef enum {
    URING_OP_NONE = 0,
    URING_OP_ACCEPT,
    URING_OP_POLL_LISTEN,
    URING_OP_READ,
    URING_OP_POLL_READ,
    URING_OP_WRITE,
    URING_OP_TIMEOUT, /* legacy — kept for migration; do NOT use for new timers */
    URING_OP_WAKEUP,
    URING_OP_CLOSE,
    URING_OP_UV_WRITE,
    URING_OP_POLL_ASYNC,
    URING_OP_POLL_SIGNAL,
    /* Differentiated timer opcodes so on_timeout knows which timer fired */
    URING_OP_TMR_READ,
    URING_OP_TMR_WRITE,
    URING_OP_TMR_IDLE,
    URING_OP_TMR_REQ,
    URING_OP_TMR_GENERIC, /**< Timer started via csilk_io_timer_start. */
} uring_op_type_t;

typedef struct {
    csilk_client_t* client;
    size_t          len;
    char            data[]; /* flexible array member — single allocation */
} uring_write_req_t;

typedef struct uring_op_context_s uring_op_context_t;

/**
 * @brief Operation context object representing a unique in-flight io_uring SQE.
 *
 * Fully eliminates the 48-bit virtual address assumption and expands the generation
 * counter from 8-bit to a 64-bit monotonically incrementing counter (preventing ABA).
 * CQE dispatch resolves the context directly in O(1) time without hash table lookup.
 */
struct uring_op_context_s {
    uint64_t generation; /**< 64-bit generation at submission time. */
    uint16_t type;       /**< uring_op_type_t operation opcode. */
    uint16_t flags;      /**< Operation flags or auxiliary state. */
    uint32_t slot_idx;   /**< Pool slot index for O(1) recycling. */
    void*    owner;      /**< Full 64-bit pointer to owner handle/client. */
    void*    data;       /**< Optional ancillary payload pointer. */
};

/* When the Submission Queue is full, submit pending entries to the kernel
 * to free SQE slots. Returns NULL only on ring-level failure. */
/**
 * @brief Acquire a submission queue entry, submitting the ring if full.
 * @param[in] ring io_uring instance to allocate from.
 * @return A valid io_uring_sqe*, or NULL only on ring-level failure after a
 *         submit-and-retry.
 * @note If io_uring_get_sqe returns NULL the ring is flushed via
 *       io_uring_submit and one more slot is requested.
 */
static inline struct io_uring_sqe*
uring_get_sqe_or_submit(struct io_uring* ring)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/**
 * @brief Allocate a fast, cache-friendly operation context from the loop's pool.
 */
static inline uring_op_context_t*
uring_op_alloc(csilk_io_loop_t* loop)
{
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return NULL;
        }
    }
    if (loop->op_free_head == 0 || !loop->op_free_stack || !loop->op_pool) {
        uring_op_context_t* ctx = calloc(1, sizeof(uring_op_context_t));
        if (ctx) {
            ctx->slot_idx = UINT32_MAX;
        }
        return ctx;
    }
    uint32_t            idx = loop->op_free_stack[--loop->op_free_head];
    uring_op_context_t* ctx = &loop->op_pool[idx];
    ctx->slot_idx = idx;
    return ctx;
}

/**
 * @brief Recycle an operation context back into the loop's pool in O(1).
 */
static inline void
uring_op_free(csilk_io_loop_t* loop, uring_op_context_t* ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->slot_idx == UINT32_MAX) {
        free(ctx);
        return;
    }
    if (!loop) {
        loop = csilk_io_default_loop();
        if (!loop) {
            return;
        }
    }
    if (loop->op_free_head < loop->op_pool_capacity && loop->op_free_stack) {
        loop->op_free_stack[loop->op_free_head++] = ctx->slot_idx;
    }
}

/**
 * @brief Encode an operation type, client pointer, and generation into user_data.
 */
static inline __u64
uring_encode_data(uring_op_type_t op, csilk_client_t* client, void* ptr)
{
    csilk_io_loop_t*    loop = client && client->owner_pool ? client->owner_pool->loop_ptr : NULL;
    uring_op_context_t* ctx = uring_op_alloc(loop);
    if (!ctx) {
        return 0;
    }
    ctx->generation = client ? client->generation : 0;
    ctx->type = (uint16_t)op;
    ctx->flags = 0;
    ctx->owner = client ? (void*)client : ptr;
    ctx->data = ptr;
    return (uint64_t)(uintptr_t)ctx;
}

/** @brief Encode timer data with 64-bit generation tracking for stale-CQE detection. */
static inline __u64
uring_encode_timer_data(uring_op_type_t op, csilk_io_timer_t* tmr)
{
    csilk_io_loop_t*    loop = tmr ? tmr->loop : NULL;
    uring_op_context_t* ctx = uring_op_alloc(loop);
    if (!ctx) {
        return 0;
    }
    ctx->generation = tmr ? tmr->generation : 0;
    ctx->type = (uint16_t)op;
    ctx->flags = 0;
    ctx->owner = tmr;
    ctx->data = NULL;
    return (uint64_t)(uintptr_t)ctx;
}

/** @brief Encode generic handle data with 64-bit generation tracking for stale-CQE detection. */
static inline __u64
uring_encode_handle_data(uring_op_type_t op, csilk_io_handle_t* handle)
{
    csilk_io_loop_t*    loop = handle ? handle->loop : NULL;
    uring_op_context_t* ctx = uring_op_alloc(loop);
    if (!ctx) {
        return 0;
    }
    ctx->generation = handle ? handle->generation : 0;
    ctx->type = (uint16_t)op;
    ctx->flags = 0;
    ctx->owner = handle;
    ctx->data = NULL;
    return (uint64_t)(uintptr_t)ctx;
}

/**
 * @brief Decode packed user_data directly via operation context pointer.
 */
static inline void
uring_decode_data(__u64 val, uring_op_type_t* op, void** ptr, uint64_t* gen)
{
    uring_op_context_t* ctx = (uring_op_context_t*)(uintptr_t)val;
    if (ctx) {
        if (op) {
            *op = (uring_op_type_t)ctx->type;
        }
        if (ptr) {
            *ptr = ctx->data ? ctx->data : ctx->owner;
        }
        if (gen) {
            *gen = ctx->generation;
        }
    } else {
        if (op) {
            *op = URING_OP_NONE;
        }
        if (ptr) {
            *ptr = NULL;
        }
        if (gen) {
            *gen = 0;
        }
    }
}

void csilk_uv_on_write_done(void* arg, ssize_t res, uint64_t gen);

/* Thread-local default loop state (defined in uring_loop.c) */
extern csilk_io_loop_t  g_default_loop;
extern int              g_default_pending;
extern int              g_default_loop_inited;
extern struct io_uring* g_default_ring_ptr;

/* Forward declarations for functions defined in uring_connection.c */
void on_read(csilk_client_t* client, ssize_t nread);
void on_write_done(void* arg, ssize_t res);
void on_timeout(csilk_client_t* client);
void client_destroy(csilk_client_t* client);
void csilk_client_close(csilk_client_t* client);
void on_close_done(csilk_client_t* client);

/* --- Thread pool for csilk_io_queue_work --- */
typedef struct uring_thread_pool_s uring_thread_pool_t;

uring_thread_pool_t* uring_tp_init(int nthreads);
void                 uring_tp_destroy(uring_thread_pool_t* tp);
int                  uring_tp_enqueue(uring_thread_pool_t*   tp,
                                      csilk_io_work_t*       work,
                                      csilk_io_work_cb       work_cb,
                                      csilk_io_after_work_cb after_cb);
void                 uring_tp_drain(uring_thread_pool_t* tp);
int                  uring_tp_wakeup_fd(uring_thread_pool_t* tp);
void                 uring_tp_set_current(uring_thread_pool_t* tp);

/* --- Deferred callback queue for sync-fallback path --- */
typedef struct uring_deferred_s {
    struct uring_deferred_s* next;
    csilk_io_work_t*         work;
    csilk_io_after_work_cb   after_cb;
    int                      status;
} uring_deferred_t;

void _uring_deferred_push(csilk_io_work_t* work, csilk_io_after_work_cb after_cb, int status);
int  _uring_deferred_drain_all(void);

/* --- Server split (uring_server.c, uring_event_loop.c) --- */

typedef struct {
    worker_pool_t*   wp;
    int              port;
    csilk_barrier_t* barrier;
} uring_worker_data_t;

typedef struct {
    csilk_io_loop_t* loop;
    csilk_io_tcp_t*  listen_handle;
    csilk_server_t*  server;
    int              worker_index;
} uring_worker_stop_data_t;

void on_stop_async(csilk_io_async_t* handle);
void on_dispatch_async(csilk_io_async_t* handle);

int uring_bind_and_listen(
    csilk_io_loop_t* loop, csilk_io_tcp_t* out_handle, int port, int backlog, bool reuseport);
void* uring_worker_thread(void* arg);
void  _csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop);

#endif
