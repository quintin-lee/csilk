#ifndef CSILK_URING_INTERNAL_H
#define CSILK_URING_INTERNAL_H

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

#endif
