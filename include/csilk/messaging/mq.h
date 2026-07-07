/**
 * @file mq.h
 * @brief In-process Message Queue (pub/sub event bus) for the csilk framework.
 *
 * Provides an in-process pub/sub system built on the I/O event loop.
 * Thread-safe publishing allows worker threads to send messages to the
 * main event loop.  Supports middleware chains, persistence via WAL, and
 * background offloading.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_MQ_H
#define CSILK_MQ_H

#include "csilk/core/types.h"

/**
 * @brief Opaque Message Queue (event bus) instance.
 *
 * Provides an in-process pub/sub system built on the I/O event loop.
 * Thread-safe publishing allows worker threads to send messages to the
 * main event loop.  Supports middleware chains, persistence via WAL, and
 * background offloading.
 */
typedef struct csilk_mq_s csilk_mq_t;

/**
 * @brief Opaque Message Queue context.
 *
 * Created per-message and passed to middleware and subscriber handlers.
 * Provides access to the topic, payload, and chain-control functions.
 * Valid only during the handler invocation — do not store the pointer.
 */
typedef struct csilk_mq_ctx_s csilk_mq_ctx_t;

/**
 * @brief MQ handler signature for middleware and subscribers.
 *
 * @param ctx  MQ context providing topic, payload, and chain control.
 */
typedef void (*csilk_mq_handler_t)(csilk_mq_ctx_t* ctx);

/**
 * @brief Signature for a background MQ worker function.
 *
 * @param topic   The topic string (valid only during the call).
 * @param payload Opaque data pointer.
 * @param len     Byte length of @p payload.
 */
typedef void (*csilk_mq_worker_t)(const char* topic, const void* payload, size_t len);

/**
 * @brief Get the Message Queue instance attached to a server.
 *
 * The MQ is created lazily on first access.
 *
 * @param server  Pointer to the server instance.
 * @return Pointer to the server's MQ, or nullptr if the server is not yet
 *         initialised.
 */
csilk_mq_t* csilk_server_get_mq(csilk_server_t* server);

/**
 * @brief Register MQ middleware for a topic.
 *
 * Middleware runs before subscribers.  Pass nullptr as @p topic to register
 * global middleware that intercepts all messages.
 *
 * @param mq         The MQ instance.
 * @param topic      Topic name to intercept, or nullptr for global middleware.
 * @param middleware Handler function.  Must not be nullptr.
 */
void csilk_mq_use(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t middleware);

/**
 * @brief Register a subscriber for a topic.
 *
 * Subscribers run after all applicable middleware (global + topic-specific)
 * has completed.
 *
 * @param mq         The MQ instance.
 * @param topic      Topic name to subscribe to.
 * @param subscriber Handler function.  Must not be nullptr.
 */
void csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber);

/**
 * @brief Publish a message to a topic.
 *
 * The payload is **copied** internally so the caller can reuse the buffer
 * immediately.  The message is enqueued and processed asynchronously on the
 * main event loop via an I/O async handle (libuv or io_uring), making this function thread-safe.
 *
 * @param mq      The MQ instance.
 * @param topic   Target topic name.
 * @param payload Pointer to the data to publish (copied internally).
 * @param len     Byte length of @p payload.
 * @return 0 on success, non-zero errno-compatible code on failure (typically
 *         ENOMEM).
 */
int csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/**
 * @brief Pass control to the next middleware or subscriber in the MQ chain.
 *
 * Must be called exactly once (or zero times if csilk_mq_abort is used)
 * for the chain to advance.
 *
 * @param ctx  The MQ context.
 */
void csilk_mq_next(csilk_mq_ctx_t* ctx);

/**
 * @brief Abort the MQ middleware/subscriber chain.
 *
 * No further handlers execute for the current message.
 *
 * @param ctx  The MQ context.
 */
void csilk_mq_abort(csilk_mq_ctx_t* ctx);

/**
 * @brief Get the topic of the current message.
 *
 * @param ctx  The MQ context.
 * @return The topic string.  Valid only for the duration of the handler call.
 */
const char* csilk_mq_get_topic(csilk_mq_ctx_t* ctx);

/**
 * @brief Get the payload of the current message.
 *
 * @param ctx      The MQ context.
 * @param[out] len Optional pointer to receive the payload byte length (may be
 * nullptr).
 * @return Pointer to the message payload.  Valid only for the duration of the
 *         handler call.  The pointer must NOT be freed.
 */
const void* csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len);

/**
 * @brief Offload message processing to a background thread.
 *
 * Hands off the current message to the thread pool for processing.
 * csilk_mq_next is called internally so the chain continues immediately.
 * The @p worker runs on a separate thread — it must be thread-safe and must
 * NOT call back into the MQ or context APIs.
 *
 * @param ctx    The MQ context.
 * @param worker Background worker function that receives the topic and
 *               a copy of the payload.
 */
void csilk_mq_offload(csilk_mq_ctx_t* ctx, csilk_mq_worker_t worker);

/**
 * @brief Message Queue statistics.
 */
typedef struct {
    uint64_t published_total; /**< Total messages published since startup. */
    uint64_t delivered_total; /**< Total messages delivered to subscribers. */
    uint64_t failed_total;    /**< Total messages that failed processing. */
    uint32_t queue_depth;     /**< Number of messages currently in memory queue. */
    uint32_t topic_count;     /**< Number of registered topics. */
} csilk_mq_stats_t;

/**
 * @brief Get current MQ statistics.
 * @param mq    The MQ instance.
 * @param stats [out] Pointer to stats struct to populate.
 */
void csilk_mq_get_stats(csilk_mq_t* mq, csilk_mq_stats_t* stats);

/**
 * @brief Convert MQ statistics to a JSON string.
 * @param stats Pointer to stats struct.
 * @return Heap-allocated JSON string (must be freed).
 */
char* csilk_mq_stats_to_json(const csilk_mq_stats_t* stats);

/**
 * @brief Register a WebSocket monitor for real-time MQ events.
 * @param mq The MQ instance.
 * @param c  Framework context (WebSocket connection).
 */
void csilk_mq_register_monitor(csilk_mq_t* mq, csilk_ctx_t* c);

/**
 * @brief Enable Write-Ahead Log (WAL) persistence for the MQ.
 *
 * When enabled, every published message is appended to @p wal_path before
 * being processed.  The WAL can be replayed on restart to recover messages.
 *
 * @param mq       The MQ instance.
 * @param wal_path File path for the WAL (e.g., "mq.wal").  The string is
 *                 copied internally.
 * @return 0 on success, non-zero on file-open failure.
 */
int csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path);

#endif /* CSILK_MQ_H */
