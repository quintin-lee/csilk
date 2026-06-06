/**
 * @file crypto.h
 * @brief Cryptographic utility functions for secure randomness and nonces.
 *
 * Provides helpers for generating cryptographically secure random bytes
 * and nonces for symmetric encryption modes like AES-256-GCM.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_CRYPTO_H
#define CSILK_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief AES-256-GCM nonce (IV) size in bytes per NIST SP 800-38D. */
static constexpr int CSILK_GCM_NONCE_SIZE = 12;

/**
 * @brief Generate a cryptographically secure random nonce for AES-256-GCM.
 *
 * This helper ensures that nonces are never reused, which is critical for
 * the security of GCM mode.  It uses the best available randomness source
 * (OS entropy or the configured crypto driver).
 *
 * @param[out] out  Buffer of at least @p len bytes.
 * @param      len  Length of the nonce to generate (typically CSILK_GCM_NONCE_SIZE).
 */
void csilk_crypto_generate_nonce(uint8_t* out, size_t len);

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 *
 * @param[out] out  Buffer to fill.
 * @param      len  Number of bytes to generate.
 * @return 0 on success, -1 on failure.
 */
int csilk_crypto_fill_random(void* out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CRYPTO_H */
