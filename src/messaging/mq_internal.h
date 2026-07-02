/**
 * @file mq_internal.h
 * @brief Internal Message Queue types — message, topic, MQ instance, and
 * dispatch context.
 *
 * Defines the internal data structures for the in-process pub/sub message
 * queue with WAL persistence.  These types are NOT part of the public API
 * and may change without notice.
 * @copyright MIT License
 */

#ifndef CSILK_MQ_TYPES_H
#define CSILK_MQ_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <csilk/core/sys_io.h>

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"
#include "csilk/core/sync.h"

/**
 * @brief Internal: A single message in the MQ linked-list queue.
 * Messages are heap-allocated and linked via @p next.
 */
typedef struct csilk_mq_msg_s {
    char*  topic;   /**< NUL-terminated topic string (heap-allocated copy). */
    void*  payload; /**< Message payload bytes (heap-allocated copy of published
                    data). */
    size_t len;     /**< Byte length of @p payload. */
    struct csilk_mq_msg_s*
        next;       /**< Pointer to the next message in the queue (nullptr for tail). */
} csilk_mq_msg_t;

/**
 * @brief Internal: A topic node in the MQ's linked list of topics.
 * Holds the topic name and its associated middleware + subscriber chain.
 */
typedef struct csilk_mq_topic_s {
    char*                    name;     /**< NUL-terminated topic name (e.g., "user.created"). */
    csilk_mq_handler_t*      handlers; /**< Dynamically-grown array of handler function
                                   pointers (middleware + subscribers). */
    size_t                   handler_count;    /**< Number of handlers currently registered. */
    size_t                   handler_capacity; /**< Allocated capacity of @p handlers. */
    struct csilk_mq_topic_s* next;             /**< Pointer to the next topic in the linked list. */
} csilk_mq_topic_t;

/**
 * @brief Internal: The Message Queue instance.
 *
 * Manages the message queue, topic registry, global middleware, and optional
 * WAL persistence.  Publishes are thread-safe (guarded by queue_mutex) and
 * are delivered asynchronously on the main event loop via a csilk_io_async_t handle.
 *
 * ## Lifecycle
 *   1. Created by _csilk_mq_new(loop).
 *   2. Topics are registered lazily on first subscribe/publish.
 *   3. Each publish enqueues a message (copied), signals the async handle,
 *      and optionally appends to the WAL.
 *   4. On the main loop, mq_dispatch processes the queue: global middleware
 *      runs first, then topic middleware, then subscribers.
 *   5. Destroyed by _csilk_mq_free() — drains the queue and frees all
 * resources.
 *
 * Not intended for direct manipulation by user code.
 */
struct csilk_mq_s {
    csilk_io_loop_t* loop;         /**< Event loop */
    csilk_io_async_t async_handle; /**< Async handle for bridging worker-thread
                                 publishes into the main loop. */
    csilk_mutex_t    queue_mutex;  /**< Mutex guarding the message linked list. */
    csilk_mq_msg_t*  queue_head;   /**< Head of the pending-message linked list. */
    csilk_mq_msg_t*  queue_tail;   /**< Tail of the pending-message linked list. */

    csilk_mq_topic_t* topics;      /**< Linked list of registered topics. */

    /* Global middlewares */
    csilk_mq_handler_t* global_middlewares; /**< Array of global middleware
                                             (intercepts every topic). */
    size_t              global_mw_count;    /**< Number of global middleware handlers registered. */
    size_t              global_mw_capacity; /**< Allocated capacity of @p global_middlewares. */

    /* Persistence (WAL) */
    csilk_io_file_t wal_fd;    /**< File descriptor for the Write-Ahead Log, or -1 if
                           disabled. */
    char*           wal_path;  /**< Path to the WAL file (heap-allocated copy, nullptr if
                           disabled). */
    csilk_mutex_t   wal_mutex; /**< Mutex guarding WAL append operations. */

    /* Monitoring */
    uint64_t published_total;       /**< Total messages published. */
    uint64_t delivered_total;       /**< Total messages delivered. */
    uint64_t failed_total;          /**< Total messages failed. */
    uint32_t queue_depth;           /**< Current messages in memory. */

    csilk_ctx_t** monitors;         /**< WebSocket monitor connections. */
    size_t        monitor_count;    /**< Number of monitors. */
    size_t        monitor_capacity; /**< Monitor array capacity. */
    csilk_mutex_t monitor_mutex;    /**< Protects monitor array. */
};

/**
 * @brief Internal: Per-message MQ context passed to middleware and subscribers.
 *
 * Contains the resolved handler chain for the current topic and tracks
 * the current position in the chain.
 */
struct csilk_mq_ctx_s {
    csilk_mq_t*         mq;            /**< Owning MQ instance. */
    csilk_mq_msg_t*     msg;           /**< The message being processed. */
    csilk_mq_handler_t* handlers;      /**< Combined handler array (global mw + topic
                                   mw + subscribers). */
    size_t              handler_count; /**< Total number of handlers in @p handlers. */
    int                 handler_index; /**< Index of the next handler to invoke. */
    int                 aborted;       /**< Non-zero if csilk_mq_abort was called. */
};

/**
 * @brief Internal: Context passed to the thread-pool work callback.
 *
 * Carries the topic, payload, and worker function pointer for background
 * message offloading.
 */
typedef struct {
    csilk_io_work_t   req;     /**< I/O work request (must be first for casting). */
    csilk_mq_worker_t handler; /**< User-provided worker function. */
    char*             topic;   /**< Topic string (heap-allocated copy). */
    void*             payload; /**< Payload data (heap-allocated copy). */
    size_t            len;     /**< Byte length of @p payload. */
} csilk_mq_work_ctx_t;

/**
 * @brief Internal: Create a new MQ instance bound to the event loop.
 *
 * @param loop  The I/O event loop (libuv or io_uring).
 * @return A new MQ instance (heap-allocated), or nullptr on failure.
 */
CSILK_INTERNAL csilk_mq_t* _csilk_mq_new(csilk_io_loop_t* loop);

/**
 * @brief Internal: Destroy an MQ instance and release all resources.
 *
 * Drains the message queue, frees topics, handlers, and the WAL file.
 *
 * @param mq  The MQ instance to free.
 */
CSILK_INTERNAL void _csilk_mq_free(csilk_mq_t* mq);

/* --- Internal WAL / dispatch (cross-file helpers) --- */
CSILK_INTERNAL int
_mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);
CSILK_INTERNAL int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);
CSILK_INTERNAL int _mq_recovery(csilk_mq_t* mq);

#endif /* CSILK_MQ_TYPES_H */
