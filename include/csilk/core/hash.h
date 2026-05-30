/**
 * @file hash.h
 * @brief Hashing primitives — SHA-1, SHA-256, HMAC-SHA256.
 *
 * Provides low-level hashing contexts and functions used internally by the
 * csilk framework for WebSocket handshake (SHA-1), JWT signing (HMAC-SHA256),
 * and general-purpose cryptographic hashing (SHA-256).
 *
 * @note SHA-1 is considered cryptographically weak for security purposes.
 *       It is used internally only for WebSocket handshake compliance
 *       (RFC 6455).  Do not use for security-critical hashing.
 * @copyright MIT License
 */

#ifndef CSILK_HASH_H
#define CSILK_HASH_H

#include <stddef.h>
#include <stdint.h>

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
	uint32_t state[5];  /**< 160-bit intermediate hash state (5 × 32-bit words). */
	uint32_t count[2];  /**< Total message length in bits (64-bit, split into two
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
void csilk_sha1_update(csilk_sha1_ctx* context, const uint8_t* data, size_t len);

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
	uint32_t state[8];  /**< 256-bit intermediate hash state (8 × 32-bit words). */
	uint64_t count;	    /**< Total message length in bits. */
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
void csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data, size_t len);

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
void csilk_hmac_sha256(
    const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]);

#endif /* CSILK_HASH_H */
