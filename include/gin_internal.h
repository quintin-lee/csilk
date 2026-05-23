/**
 * @file gin_internal.h
 * @brief Internal header for SHA1, Base64, and WebSocket frame parsing.
 * MIT License
 */

#ifndef GIN_INTERNAL_H
#define GIN_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

/** @brief SHA1 hashing context. */
typedef struct {
    uint32_t state[5];   /**< Intermediate hash state. */
    uint32_t count[2];   /**< Message length counter. */
    uint8_t buffer[64];   /**< Data block buffer. */
} gin_sha1_ctx;

/** @brief Initialize a SHA1 context.
 * @param context SHA1 context to initialize. */
void gin_sha1_init(gin_sha1_ctx* context);

/** @brief Feed data into the SHA1 hashing context.
 * @param context SHA1 context.
 * @param data Input data bytes.
 * @param len Length of input data. */
void gin_sha1_update(gin_sha1_ctx* context, const uint8_t* data, uint32_t len);

/** @brief Finalize SHA1 hash and produce the digest.
 * @param context SHA1 context.
 * @param digest Output buffer for 20-byte hash. */
void gin_sha1_final(gin_sha1_ctx* context, uint8_t digest[20]);

/** @brief Encode raw bytes as Base64 string.
 * @param src Source byte buffer.
 * @param len Length of source data.
 * @param out Output buffer (must be at least ((len+2)/3)*4+1 bytes). */
void gin_base64_encode(const uint8_t* src, size_t len, char* out);

/** @brief Parse incoming WebSocket frame data.
 * @param c Request context.
 * @param buf Raw input buffer.
 * @param nread Number of bytes read. */
void gin_ws_parse_frame(gin_ctx_t* c, const uint8_t* buf, size_t nread);

#endif
