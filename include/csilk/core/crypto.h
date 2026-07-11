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
enum { CSILK_GCM_NONCE_SIZE = 12 };

/**
 * @brief Supported JWT signing algorithms.
 *
 * HS256 uses HMAC-SHA256 with a symmetric key (char* secret).
 * RS256 uses RSA PKCS1-v1_5 with SHA-256 and a PEM-encoded private key.
 * ES256 uses ECDSA P-256 with SHA-256 and a PEM-encoded EC private key.
 */
typedef enum {
    CSILK_JWT_HS256 = 0, /**< HMAC-SHA256 (symmetric). */
    CSILK_JWT_RS256 = 1, /**< RSA PKCS1-v1_5 + SHA-256. */
    CSILK_JWT_ES256 = 2  /**< ECDSA P-256 + SHA-256. */
} csilk_jwt_alg_t;

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

/**
 * @brief Pluggable cryptographic primitive driver.
 *
 * Allows users to replace the default software implementations of SHA256,
 * HMAC-SHA256, and UUID generation (e.g., with hardware-accelerated or
 * FIPS-compliant versions).  All function pointers must be non-nullptr.
 */
typedef struct {
    /** @brief Compute the SHA-256 hash of a buffer.
   *  @param data  Input data.
   *  @param len   Input length.
   *  @param[out] out  32-byte hash output. */
    void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
    /** @brief Compute HMAC-SHA256.
   *  @param key       HMAC key.
   *  @param key_len   Key length.
   *  @param data      Input data.
   *  @param data_len  Input length.
   *  @param[out] out  32-byte HMAC output. */
    void (*hmac_sha256)(
        const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]);
    /** @brief Generate a random version-4 UUID string.
   *  @param[out] buf  Output buffer of at least 37 bytes.  Populated with a
   *                   NUL-terminated UUID string. */
    void (*generate_uuid)(char buf[37]);
    /** @brief Fill a buffer with cryptographically secure random bytes.
   *  @param[out] out  Buffer to fill.
   *  @param      len  Number of bytes to generate.
   *  @return 0 on success, -1 on failure. */
    int (*fill_random)(void* out, size_t len);
} csilk_crypto_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CRYPTO_H */
