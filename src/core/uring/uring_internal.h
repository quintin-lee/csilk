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
 * shared across the uring/*.c translation units.
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
    URING_OP_ACCEPT,
    URING_OP_READ,
    URING_OP_WRITE,
    URING_OP_TIMEOUT, /* legacy — kept for migration; do NOT use for new timers */
    URING_OP_WAKEUP,
    URING_OP_CLOSE,
    URING_OP_UV_WRITE,
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

typedef struct {
    uring_op_type_t op;
    void*           ptr; // pointer to client or server
} uring_sqe_data_t;

// Helper to encode type and ptr into __u64
/**
 * @brief Encode an operation type, client pointer, and generation into user_data.
 * @param[in] op      io_uring opcode (stored in the top 8 bits).
 * @param[in] client  Client owning the op, or NULL; its generation occupies bits 48-55.
 * @param[in] ptr     Opaque pointer (client/server) stored in the low 48 bits.
 * @return Packed 64-bit value suitable for io_uring_sqe_set_data64.
 * @note The pointer is masked to 48 bits; only the low 48 bits of ptr are kept.
 */
static inline __u64
uring_encode_data(uring_op_type_t op, csilk_client_t* client, void* ptr)
{
    uint64_t val = (uint64_t)ptr;
    val &= 0x0000FFFFFFFFFFFFULL;
    val |= ((uint64_t)op) << 56;
    if (client) {
        uint64_t gen = client->generation;
        val |= (gen << 48);
    }
    return val;
}

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

/** @brief Encode timer data with generation tracking for stale-CQE detection. */
static inline __u64
uring_encode_timer_data(uring_op_type_t op, csilk_io_timer_t* tmr)
{
    uint64_t val = (uint64_t)(void*)tmr;
    val &= 0x0000FFFFFFFFFFFFULL;
    val |= ((uint64_t)op) << 56;
    val |= ((uint64_t)tmr->generation) << 48;
    return val;
}

/**
 * @brief Decode packed operation data into its components.
 * @param[in]  val  Packed 64-bit user_data produced by uring_encode_data or
 *                  uring_encode_timer_data.
 * @param[out] op   Receives the operation type (top 8 bits).
 * @param[out] ptr  Receives the low-48-bit opaque pointer.
 * @param[out] gen  Receives the 8-bit generation, or NULL to ignore it.
 */
static inline void
uring_decode_data(__u64 val, uring_op_type_t* op, void** ptr, uint8_t* gen)
{
    *op = (uring_op_type_t)(val >> 56);
    *ptr = (void*)(val & 0x0000FFFFFFFFFFFFULL);
    if (gen) {
        *gen = (uint8_t)((val >> 48) & 0xFF);
    }
}

void csilk_uv_on_write_done(void* arg, ssize_t res);

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
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             count;
    int             waiting;
} csilk_barrier_t;

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

void uring_barrier_init(csilk_barrier_t* b, int count);
void uring_barrier_wait(csilk_barrier_t* b);
void uring_barrier_destroy(csilk_barrier_t* b);

void on_signal(csilk_server_t* server);
void on_stop_async(csilk_io_async_t* handle);
void on_dispatch_async(csilk_io_async_t* handle);

int uring_bind_and_listen(
    csilk_io_loop_t* loop, csilk_io_tcp_t* out_handle, int port, int backlog, bool reuseport);
void* uring_worker_thread(void* arg);
void  _csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop);

#endif
