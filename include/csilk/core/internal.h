/**
 * @file internal.h
 * @brief Internal framework primitives — crypto, codec, MQ, and dispatch.
 *
 * This header exposes functions and types used internally by the csilk
 * framework.  They are NOT part of the public API and may change without
 * notice.  External consumers should not rely on them.
 *
 * ## Contents
 *   - **Hashing**: SHA-1 (WebSocket handshake), SHA-256, HMAC-SHA256.
 *   - **Encoding**: Base64, Base64URL, URL percent-decoding.
 *   - **WebSocket**: Frame parsing and dispatch for RFC 6455.
 *   - **Message Queue**: In-process pub/sub with WAL persistence,
 *     threading via libuv async handles and mutexes.
 *   - **Crypto dispatch**: Weak-symbol stubs (_csilk_send_response,
 *     _csilk_symmetric_encrypt, etc.) that route through the server's
 *     crypto/cipher driver if one is installed, or fall back to built-in
 *     software implementations.
 * @copyright MIT License
 */

#ifndef CSILK_INTERNAL_H
#define CSILK_INTERNAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

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
 * @brief Internal: Symmetric encrypt using the context's cipher driver
 * or the built-in OpenSSL AES-256-GCM implementation.
 *
 * @param c              Request context (for driver lookup, may be NULL).
 * @param key            Encryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param plaintext      Data to encrypt.
 * @param plaintext_len  Plaintext length.
 * @param iv             12-byte initialisation vector (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param[out] ciphertext  Output buffer (must be at least plaintext_len bytes).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext length.
 * @param[out] tag       16-byte authentication tag buffer.
 * @param tag_len        Tag buffer size (must be 16).
 * @return 0 on success, -1 on failure.
 */
int _csilk_symmetric_encrypt(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                             const uint8_t* plaintext, size_t plaintext_len,
                             const uint8_t* iv, size_t iv_len,
                             uint8_t* ciphertext, size_t* ciphertext_len,
                             uint8_t* tag, size_t tag_len);

/**
 * @brief Internal: Symmetric decrypt using the context's cipher driver
 * or the built-in OpenSSL AES-256-GCM implementation.
 *
 * @param c              Request context (for driver lookup, may be NULL).
 * @param key            Decryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param ciphertext     Data to decrypt.
 * @param ciphertext_len Ciphertext length.
 * @param iv             12-byte initialisation vector (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param tag            16-byte authentication tag.
 * @param tag_len        Tag length (must be 16).
 * @param[out] plaintext   Output buffer (must be at least ciphertext_len
 * bytes).
 * @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
 * @return 0 on success, -1 on failure (including tag mismatch).
 */
int _csilk_symmetric_decrypt(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                             const uint8_t* ciphertext, size_t ciphertext_len,
                             const uint8_t* iv, size_t iv_len,
                             const uint8_t* tag, size_t tag_len,
                             uint8_t* plaintext, size_t* plaintext_len);

/**
 * @brief Internal: Generate an RSA-2048 key pair using the context's cipher
 * driver or the built-in OpenSSL implementation.
 *
 * Keys are output as PEM-encoded strings.
 *
 * @param c            Request context (for driver lookup, may be NULL).
 * @param[out] public_key   PEM public key buffer.
 * @param[in,out] pub_len   In: capacity, Out: actual PEM length (incl. NUL).
 * @param[out] private_key  PEM private key buffer.
 * @param[in,out] priv_len  In: capacity, Out: actual PEM length (incl. NUL).
 * @return 0 on success, -1 on failure.
 */
int _csilk_generate_keypair(csilk_ctx_t* c, char* public_key, size_t* pub_len,
                            char* private_key, size_t* priv_len);

/**
 * @brief Internal: Asymmetric encrypt using the context's cipher driver
 * or the built-in OpenSSL RSA-OAEP implementation.
 *
 * @param c              Request context (for driver lookup, may be NULL).
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Public key length.
 * @param plaintext      Data to encrypt (max ~190 bytes for RSA-2048).
 * @param plaintext_len  Plaintext length.
 * @param[out] ciphertext  256-byte output buffer.
 * @param[in,out] ciphertext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
int _csilk_asymmetric_encrypt(csilk_ctx_t* c, const char* public_key,
                              size_t pub_len, const uint8_t* plaintext,
                              size_t plaintext_len, uint8_t* ciphertext,
                              size_t* ciphertext_len);

/**
 * @brief Internal: Asymmetric decrypt using the context's cipher driver
 * or the built-in OpenSSL RSA-OAEP implementation.
 *
 * @param c              Request context (for driver lookup, may be NULL).
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Private key length.
 * @param ciphertext     Data to decrypt (typically 256 bytes for RSA-2048).
 * @param ciphertext_len Ciphertext length.
 * @param[out] plaintext   Output buffer.
 * @param[in,out] plaintext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
int _csilk_asymmetric_decrypt(csilk_ctx_t* c, const char* private_key,
                              size_t priv_len, const uint8_t* ciphertext,
                              size_t ciphertext_len, uint8_t* plaintext,
                              size_t* plaintext_len);

/**
 * @brief Internal: Sign data using the context's cipher driver
 * or the built-in OpenSSL RSA-PSS implementation.
 *
 * @param c            Request context (for driver lookup, may be NULL).
 * @param private_key  PEM-encoded RSA private key.
 * @param priv_len     Private key length.
 * @param data         Data to sign.
 * @param data_len     Data length.
 * @param[out] signature  256-byte signature buffer.
 * @param[in,out] sig_len  In: capacity, Out: actual signature length.
 * @return 0 on success, -1 on failure.
 */
int _csilk_sign(csilk_ctx_t* c, const char* private_key, size_t priv_len,
                const uint8_t* data, size_t data_len, uint8_t* signature,
                size_t* sig_len);

/**
 * @brief Internal: Verify a signature using the context's cipher driver
 * or the built-in OpenSSL RSA-PSS implementation.
 *
 * @param c           Request context (for driver lookup, may be NULL).
 * @param public_key  PEM-encoded RSA public key.
 * @param pub_len     Public key length.
 * @param data        Original signed data.
 * @param data_len    Data length.
 * @param signature   Signature to verify.
 * @param sig_len     Signature length.
 * @return 0 on valid signature, -1 on invalid or error.
 */
int _csilk_verify(csilk_ctx_t* c, const char* public_key, size_t pub_len,
                  const uint8_t* data, size_t data_len,
                  const uint8_t* signature, size_t sig_len);

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
 * WAL persistence.  Publishes are thread-safe (guarded by queue_mutex) and
 * are delivered asynchronously on the main event loop via a uv_async_t handle.
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
