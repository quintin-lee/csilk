/**
 * @file csilk_internal.h
 * @brief Internal header for SHA1, SHA256, HMAC, Base64, UUID, URL decoding,
 *        WebSocket frame parsing, and the Message Queue implementation.
 *
 * This header exposes functions and types that are used internally by the
 * csilk framework.  They are not part of the public API and may change
 * without notice.  External consumers should not rely on them.
 * @copyright MIT License
 */

#ifndef CSILK_INTERNAL_H
#define CSILK_INTERNAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "csilk.h"
#include "csilk_test.h"

/**
 * @brief SHA-1 hashing context.
 *
 * Holds intermediate state and buffered data during a multi-step SHA-1
 * computation.  Use csilk_sha1_init / _update / _final.
 *
 * @note SHA-1 is considered cryptographically weak for security purposes.
 *       It is used internally only for WebSocket handshake compliance
 *       (RFC 6455).  Do not use for security-critical hashing.
 */
typedef struct {
  uint32_t state[5]; /**< 160-bit intermediate hash state (5 × 32-bit words). */
  uint32_t count[2]; /**< Total message length in bits (64-bit, split into two
                        32-bit halves). */
  uint8_t buffer[64]; /**< 512-bit block buffer for data not yet processed. */
} csilk_sha1_ctx;

/**
 * @brief Initialise a SHA-1 hashing context.
 *
 * Must be called before the first csilk_sha1_update.
 *
 * @param context  Pointer to an uninitialised csilk_sha1_ctx.
 */
void csilk_sha1_init(csilk_sha1_ctx* context);

/**
 * @brief Feed data into the SHA-1 hashing context.
 *
 * Can be called multiple times with arbitrary-length inputs.
 *
 * @param context  SHA-1 context (initialised).
 * @param data     Input bytes.
 * @param len      Number of bytes in @p data.
 */
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data,
                       size_t len);

/**
 * @brief Finalise the SHA-1 hash and write the 20-byte digest.
 *
 * After this call the context should not be used without re-initialising.
 *
 * @param context  SHA-1 context.
 * @param[out] digest  20-byte array receiving the SHA-1 hash.
 */
void csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20]);

/**
 * @brief SHA-256 hashing context.
 *
 * Holds intermediate state and buffered data during a multi-step SHA-256
 * computation.  Use csilk_sha256_init / _update / _final.
 */
typedef struct {
  uint32_t state[8]; /**< 256-bit intermediate hash state (8 × 32-bit words). */
  uint64_t count;    /**< Total message length in bits. */
  uint8_t buffer[64]; /**< 512-bit block buffer for data not yet processed. */
} csilk_sha256_ctx;

/**
 * @brief Initialise a SHA-256 hashing context.
 *
 * @param context  Pointer to an uninitialised csilk_sha256_ctx.
 */
void csilk_sha256_init(csilk_sha256_ctx* context);

/**
 * @brief Feed data into the SHA-256 hashing context.
 *
 * @param context  SHA-256 context (initialised).
 * @param data     Input bytes.
 * @param len      Number of bytes in @p data.
 */
void csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data,
                         size_t len);

/**
 * @brief Finalise the SHA-256 hash and write the 32-byte digest.
 *
 * @param context  SHA-256 context.
 * @param[out] digest  32-byte array receiving the SHA-256 hash.
 */
void csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32]);

/**
 * @brief Compute HMAC-SHA256 (keyed-hash message authentication code).
 *
 * Implements RFC 2104 using SHA-256 as the underlying hash function.
 *
 * @param key       Secret key bytes.
 * @param key_len   Byte length of @p key.
 * @param data      Input data to authenticate.
 * @param data_len  Byte length of @p data.
 * @param[out] out  32-byte buffer receiving the HMAC output.
 */
void csilk_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                       size_t data_len, uint8_t out[32]);

/**
 * @brief Encode raw bytes as a standard Base64 string.
 *
 * Produces a NUL-terminated Base64 string per RFC 4648 §4.
 *
 * @param src  Source byte buffer.
 * @param len  Byte length of @p src.
 * @param[out] out  Output buffer.  Must be at least ((len + 2) / 3) * 4 + 1
 *                  bytes to hold the encoded output plus NUL terminator.
 */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out);

/**
 * @brief Encode raw bytes as a Base64URL (RFC 4648 §5) string.
 *
 * Like Base64 but uses '-' and '_' instead of '+' and '/', and omits
 * padding '=' characters.
 *
 * @param src  Source byte buffer.
 * @param len  Byte length of @p src.
 * @param[out] out  Output buffer (must be large enough for the encoded string).
 */
void csilk_base64url_encode(const uint8_t* src, size_t len, char* out);

/**
 * @brief Decode a Base64URL (RFC 4648 §5) string to raw bytes.
 *
 * Handles both padded and unpadded input.
 *
 * @param src  NUL-terminated Base64URL string.
 * @param[out] out  Output buffer for decoded bytes (must be at least
 *                  strlen(src) * 3 / 4 bytes).
 * @return The number of decoded bytes on success, or -1 if the input
 *         contains invalid characters or the length is invalid.
 */
int csilk_base64url_decode(const char* src, uint8_t* out);

/**
 * @brief Parse an incoming WebSocket frame from the raw TCP stream.
 *
 * Processes one or more frames from the receive buffer, dispatches data
 * frames to the registered on_message callback, and handles control frames
 * (ping/pong/close) internally.
 *
 * @param c     Request context (WebSocket mode must be active).
 * @param buf   Raw bytes received from the socket.
 * @param nread Number of bytes in @p buf.
 */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread);

/**
 * @brief Internal: Trigger the response send path.
 *
 * Weak symbol so that tests or custom builds can override it.  Called
 * when an async handler finishes and the response should be flushed.
 *
 * @param c  Request context.
 */
void _csilk_send_response(csilk_ctx_t* c) __attribute__((weak));

/**
 * @brief Internal: Send data through the appropriate I/O path.
 *
 * Routes data through the TLS wrapper if TLS is enabled, or writes directly
 * to the TCP socket otherwise.
 *
 * @param c    Request context.
 * @param data Bytes to send.
 * @param len  Number of bytes to send.
 */
void _csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len);

/**
 * @brief Internal: Compute HMAC-SHA256 using the server's crypto driver (if
 * set) or the built-in software implementation.
 *
 * @param c        Request context (for driver lookup).
 * @param key      HMAC key.
 * @param key_len  Key length.
 * @param data     Input data.
 * @param data_len Input length.
 * @param[out] out 32-byte HMAC output.
 */
void _csilk_hmac_sha256(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len, uint8_t out[32]);

/**
 * @brief Internal: Generate a random UUID v4 string using the crypto driver
 * (if set) or the built-in /dev/urandom-based implementation.
 *
 * @param c   Request context (for driver lookup).
 * @param[out] buf  Output buffer of at least 37 bytes.  Receives a
 *                  NUL-terminated UUID string (e.g.,
 *                  "f81d4fae-7dec-11d0-a765-00a0c91e6bf6").
 */
void _csilk_generate_uuid(csilk_ctx_t* c, char buf[37]);

/**
 * @brief URL-decode a percent-encoded string in-place.
 *
 * Converts %XX sequences to their byte values and '+' to space.
 * The decoded string is always shorter than or equal to the input.
 *
 * @param str  NUL-terminated percent-encoded string (modified in-place).
 * @return The length of the decoded string (may be shorter than strlen
 *         of the original).
 */
size_t csilk_url_decode(char* str);

/**
 * @brief Generate a random UUID v4 string (standalone, no context needed).
 *
 * Uses /dev/urandom or an equivalent OS entropy source.
 *
 * @param[out] buf  Output buffer of at least 37 bytes.  Populated with a
 *                  NUL-terminated UUID string.
 */
void csilk_generate_uuid(char* buf);

/**
 * @brief Internal: A single message in the MQ linked-list queue.
 * Messages are heap-allocated and linked via @p next.
 */
typedef struct csilk_mq_msg_s {
  char* topic;   /**< NUL-terminated topic string (heap-allocated copy). */
  void* payload; /**< Message payload bytes (heap-allocated copy of published
                    data). */
  size_t len;    /**< Byte length of @p payload. */
  struct csilk_mq_msg_s*
      next; /**< Pointer to the next message in the queue (NULL for tail). */
} csilk_mq_msg_t;

/**
 * @brief Internal: A topic node in the MQ's linked list of topics.
 * Holds the topic name and its associated middleware + subscriber chain.
 */
typedef struct csilk_mq_topic_s {
  char* name; /**< NUL-terminated topic name (e.g., "user.created"). */
  csilk_mq_handler_t* handlers; /**< Dynamically-grown array of handler function
                                   pointers (middleware + subscribers). */
  size_t handler_count;         /**< Number of handlers currently registered. */
  size_t handler_capacity;      /**< Allocated capacity of @p handlers. */
  struct csilk_mq_topic_s*
      next; /**< Pointer to the next topic in the linked list. */
} csilk_mq_topic_t;

/**
 * @brief Internal: The Message Queue instance.
 *
 * Manages the message queue, topic registry, global middleware, and optional
 * WAL persistence.  Not intended for direct manipulation by user code.
 */
struct csilk_mq_s {
  uv_async_t async_handle;    /**< libuv async handle for bridging worker-thread
                                 publishes into the main loop. */
  uv_mutex_t queue_mutex;     /**< Mutex guarding the message linked list. */
  csilk_mq_msg_t* queue_head; /**< Head of the pending-message linked list. */
  csilk_mq_msg_t* queue_tail; /**< Tail of the pending-message linked list. */

  csilk_mq_topic_t* topics; /**< Linked list of registered topics. */

  /* Global middlewares */
  csilk_mq_handler_t* global_middlewares; /**< Array of global middleware
                                             (intercepts every topic). */
  size_t
      global_mw_count; /**< Number of global middleware handlers registered. */
  size_t
      global_mw_capacity; /**< Allocated capacity of @p global_middlewares. */

  /* Persistence (WAL) */
  uv_file wal_fd;       /**< File descriptor for the Write-Ahead Log, or -1 if
                           disabled. */
  char* wal_path;       /**< Path to the WAL file (heap-allocated copy, NULL if
                           disabled). */
  uv_mutex_t wal_mutex; /**< Mutex guarding WAL append operations. */
};

/**
 * @brief Internal: Per-message MQ context passed to middleware and subscribers.
 *
 * Contains the resolved handler chain for the current topic and tracks
 * the current position in the chain.
 */
struct csilk_mq_ctx_s {
  csilk_mq_t* mq;               /**< Owning MQ instance. */
  csilk_mq_msg_t* msg;          /**< The message being processed. */
  csilk_mq_handler_t* handlers; /**< Combined handler array (global mw + topic
                                   mw + subscribers). */
  size_t handler_count;         /**< Total number of handlers in @p handlers. */
  int handler_index;            /**< Index of the next handler to invoke. */
  int aborted;                  /**< Non-zero if csilk_mq_abort was called. */
};

/**
 * @brief Internal: Context passed to libuv's thread-pool work callback.
 *
 * Carries the topic, payload, and worker function pointer for background
 * message offloading.
 */
typedef struct {
  uv_work_t req; /**< libuv work request (must be first for casting). */
  csilk_mq_worker_t handler; /**< User-provided worker function. */
  char* topic;               /**< Topic string (heap-allocated copy). */
  void* payload;             /**< Payload data (heap-allocated copy). */
  size_t len;                /**< Byte length of @p payload. */
} csilk_mq_work_ctx_t;

/**
 * @brief Internal: Create a new MQ instance bound to a libuv loop.
 *
 * @param loop  The libuv event loop.
 * @return A new MQ instance (heap-allocated), or NULL on failure.
 */
csilk_mq_t* _csilk_mq_new(uv_loop_t* loop);

/**
 * @brief Internal: Destroy an MQ instance and release all resources.
 *
 * Drains the message queue, frees topics, handlers, and the WAL file.
 *
 * @param mq  The MQ instance to free.
 */
void _csilk_mq_free(csilk_mq_t* mq);

#endif
