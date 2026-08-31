#ifndef CSILK_ASYNC_H
#define CSILK_ASYNC_H

/**
 * @file async.h
 * @brief Managed asynchronous operation lifecycle and generation safety (Anti-ABA).
 *
 * Provides csilk_async_op_t to encapsulate background asynchronous tasks with:
 * - Worker thread dispatch
 * - Generational validity checks preventing Use-After-Free/ABA on recycled connections
 * - Optional timeout timers with automatic gateway timeout response
 * - Automatic connection reference counting
 */

#include <stddef.h>
#include <stdint.h>
#include "csilk/core/ctx/context.h"
#include "csilk/core/sys_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_async_op_s csilk_async_op_t;

/**
 * @brief Callback signature for completed async operations.
 * @param c       The request context.
 * @param result  User-provided result pointer.
 */
typedef void (*csilk_async_cb)(csilk_ctx_t* c, void* result);

/**
 * @brief Callback signature for timed-out async operations.
 * @param c       The request context.
 */
typedef void (*csilk_async_timeout_cb)(csilk_ctx_t* c);

/**
 * @brief Structured asynchronous operation handle.
 */
struct csilk_async_op_s {
    csilk_ctx_t*     ctx;         /**< Associated request context. */
    uint64_t         generation;  /**< Generation tag of the owning connection. */
    uint64_t         request_seq; /**< Request sequence generation tag. */
    uint64_t         stream_gen;  /**< Stream generation tag (for HTTP/2 multiplexed streams). */
    csilk_io_timer_t timer;       /**< Timeout timer handle. */
    csilk_async_cb   on_complete; /**< Completion callback. */
    csilk_async_timeout_cb on_timeout; /**< Timeout callback. */
    void*                  user_data;  /**< Arbitrary user payload. */
    _Atomic int            completed;  /**< 1 if completed, cancelled, or timed out (atomic CAS). */
    _Atomic int32_t        ref_count;  /**< Atomic reference count for op memory lifetime. */
    int                    timer_armed;  /**< 1 if timeout timer was started. */
    int                    timer_closed; /**< 1 if timer close has been requested. */
};

/**
 * @brief Begin a managed asynchronous operation on a request context.
 *
 * Automatically marks context as async, increments connection reference count,
 * and arms an optional timeout timer.
 *
 * @param c             The request context.
 * @param timeout_ms    Timeout in milliseconds (0 = no timeout).
 * @param on_complete   Completion callback invoked on worker thread.
 * @param on_timeout    Timeout callback invoked on worker thread (may be NULL for default 504).
 * @param user_data     Optional user context payload.
 * @return Allocated and initialized async operation handle, or NULL on error.
 */
csilk_async_op_t* csilk_async_op_begin(csilk_ctx_t*           c,
                                       uint64_t               timeout_ms,
                                       csilk_async_cb         on_complete,
                                       csilk_async_timeout_cb on_timeout,
                                       void*                  user_data);

/**
 * @brief Complete a managed asynchronous operation with a result.
 *
 * Safely dispatches completion callback to the owner worker thread, validates
 * connection generation tag, stops the timer, and decrements connection ref count.
 *
 * @param op     The async operation handle.
 * @param result Pointer to the result payload passed to on_complete.
 * @return 0 on success, negative if already completed/stale.
 */
int csilk_async_op_complete(csilk_async_op_t* op, void* result);

/**
 * @brief Cancel a managed asynchronous operation without invoking completion callback.
 *
 * Disarms the timer and releases the connection reference.
 *
 * @param op The async operation handle.
 * @return 0 on success, negative if already completed.
 */
int csilk_async_op_cancel(csilk_async_op_t* op);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_ASYNC_H */
