/**
 * @file csilk_internal.h
 * @brief Internal header for SHA1, Base64, and WebSocket frame parsing.
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

/** @brief SHA1 hashing context. */
typedef struct {
  uint32_t state[5];  /**< Intermediate hash state. */
  uint32_t count[2];  /**< Message length counter. */
  uint8_t buffer[64]; /**< Data block buffer. */
} csilk_sha1_ctx;

/** @brief Initialize a SHA1 context.
 * @param context SHA1 context to initialize. */
void csilk_sha1_init(csilk_sha1_ctx* context);

/** @brief Feed data into the SHA1 hashing context.
 * @param context SHA1 context.
 * @param data Input data bytes.
 * @param len Length of input data. */
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data,
                       size_t len);

/** @brief Finalize SHA1 hash and produce the digest.
 * @param context SHA1 context.
 * @param digest Output buffer for 20-byte hash. */
void csilk_sha1_final(csilk_sha1_ctx* context, uint8_t digest[20]);

/** @brief SHA256 hashing context. */
typedef struct {
  uint32_t state[8];  /**< Intermediate hash state. */
  uint64_t count;     /**< Message length in bits. */
  uint8_t buffer[64]; /**< Data block buffer. */
} csilk_sha256_ctx;

/** @brief Initialize a SHA256 context.
 * @param context SHA256 context. */
void csilk_sha256_init(csilk_sha256_ctx* context);

/** @brief Feed data into the SHA256 context.
 * @param context SHA256 context.
 * @param data Input data.
 * @param len Data length. */
void csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data,
                         size_t len);

/** @brief Finalize SHA256 hash and produce digest.
 * @param context SHA256 context.
 * @param digest Output buffer (32 bytes). */
void csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32]);

/** @brief Compute HMAC-SHA256.
 * @param key Secret key.
 * @param key_len Key length.
 * @param data Input data.
 * @param data_len Data length.
 * @param out Output buffer (32 bytes). */
void csilk_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                       size_t data_len, uint8_t out[32]);

/** @brief Encode raw bytes as Base64 string.
 * @param src Source byte buffer.
 * @param len Length of source data.
 * @param out Output buffer (must be at least ((len+2)/3)*4+1 bytes). */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out);

/** @brief Encode raw bytes as Base64URL string (RFC 4648).
 * @param src Source byte buffer.
 * @param len Length of source data.
 * @param out Output buffer. */
void csilk_base64url_encode(const uint8_t* src, size_t len, char* out);

/** @brief Decode a Base64URL string.
 * @param src Base64URL string.
 * @param out Output buffer.
 * @return Decoded length, or -1 on error. */
int csilk_base64url_decode(const char* src, uint8_t* out);

/** @brief Parse incoming WebSocket frame data.
 * @param c Request context.
 * @param buf Raw input buffer.
 * @param nread Number of bytes read. */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread);

/** @brief Internal: Trigger response sending (used for async offloading).
 * @param c Request context. */
void _csilk_send_response(csilk_ctx_t* c) __attribute__((weak));

/** @brief Internal: Send data through TLS if enabled, or direct.
 * @param c Request context.
 * @param data Data to send.
 * @param len Data length. */
void _csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len);

/** @brief Internal: Compute HMAC-SHA256 (uses driver if set).
 * @param c Request context.
 * @param key Key for HMAC.
 * @param key_len Length of the key.
 * @param data Data to hash.
 * @param data_len Length of data.
 * @param out Output buffer for 32-byte hash. */
void _csilk_hmac_sha256(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len, uint8_t out[32]);

/** @brief Internal: Generate UUID v4 (uses driver if set).
 * @param c Request context.
 * @param buf Output buffer for 37-byte string. */
void _csilk_generate_uuid(csilk_ctx_t* c, char buf[37]);

/** @brief URL decode a string in-place.
 * @param str The string to decode.
 * @return The length of the decoded string. */
size_t csilk_url_decode(char* str);

/** @brief Generate a random UUID v4 string.
 * @param buf Output buffer (at least 37 bytes). */
void csilk_generate_uuid(char* buf);

typedef struct csilk_mq_msg_s {
  char* topic;
  void* payload;
  size_t len;
  struct csilk_mq_msg_s* next;
} csilk_mq_msg_t;

typedef struct csilk_mq_topic_s {
  char* name;
  csilk_mq_handler_t* handlers; /* Middlewares + Subscribers */
  size_t handler_count;
  size_t handler_capacity;
  struct csilk_mq_topic_s* next;
} csilk_mq_topic_t;

struct csilk_mq_s {
  uv_async_t async_handle;
  uv_mutex_t queue_mutex;
  csilk_mq_msg_t* queue_head;
  csilk_mq_msg_t* queue_tail;

  csilk_mq_topic_t* topics; /* Linked list of topics */

  /* Global middlewares */
  csilk_mq_handler_t* global_middlewares;
  size_t global_mw_count;
  size_t global_mw_capacity;
};

struct csilk_mq_ctx_s {
  csilk_mq_t* mq;
  csilk_mq_msg_t* msg;
  csilk_mq_handler_t* handlers;
  size_t handler_count;
  int handler_index;
  int aborted;
};

typedef struct {
  uv_work_t req;
  csilk_mq_worker_t handler; /* Use worker signature */
  char* topic;
  void* payload;
  size_t len;
} csilk_mq_work_ctx_t;

/** @brief Internal Init and Free */
csilk_mq_t* _csilk_mq_new(uv_loop_t* loop);
void _csilk_mq_free(csilk_mq_t* mq);

#endif
