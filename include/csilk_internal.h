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

/** @brief Encode raw bytes as Base64 string.
 * @param src Source byte buffer.
 * @param len Length of source data.
 * @param out Output buffer (must be at least ((len+2)/3)*4+1 bytes). */
void csilk_base64_encode(const uint8_t* src, size_t len, char* out);

/** @brief Parse incoming WebSocket frame data.
 * @param c Request context.
 * @param buf Raw input buffer.
 * @param nread Number of bytes read. */
void csilk_ws_parse_frame(csilk_ctx_t* c, const uint8_t* buf, size_t nread);

/** @brief Internal: Trigger response sending (used for async offloading).
 * @param c Request context. */
void _csilk_send_response(csilk_ctx_t* c);

/** @brief URL decode a string in-place.
 * @param str The string to decode.
 * @return The length of the decoded string. */
size_t csilk_url_decode(char* str);

/** @brief Generate a random UUID v4 string.
 * @param buf Output buffer (at least 37 bytes). */
void csilk_generate_uuid(char* buf);

#endif
