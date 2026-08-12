#pragma once
/**
 * @file bcrypt.h
 * @brief bcrypt password hashing implementation.
 *
 * Implements the bcrypt (OpenBSD) password hashing function based on
 * the Eksblowfish key schedule and Blowfish cipher.  Produced hashes
 * are compatible with the $2a$ format used by OpenBSD and most
 * existing bcrypt implementations.
 *
 * ## Format
 *
 * bcrypt hashes have the form:
 * ```
 * $2a$XX$<22-char-cost><16-char-salt><31-char-checksum>
 * ```
 * where XX is a two-digit cost factor (04–31) and each character of
 * the 53-character suffix is drawn from the bcrypt base64 alphabet
 * (`./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789`).
 *
 * ## API
 *
 * | Function | Purpose |
 * |---|---|
 * | `csilk_bcrypt_hash()` | Hash a password with a given cost |
 * | `csilk_bcrypt_verify()` | Verify a password against a bcrypt hash |
 *
 * @note Passwords longer than 72 bytes are silently truncated — this is
 *       intentional and matches the upstream OpenBSD behaviour.
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum bcrypt cost factor (2^31 rounds). */
enum { CSILK_BCRYPT_MAX_COST = 31 };

/** @brief Minimum bcrypt cost factor (2^4 rounds). */
enum { CSILK_BCRYPT_MIN_COST = 4 };

/** @brief Default bcrypt cost factor. */
enum { CSILK_BCRYPT_DEFAULT_COST = 12 };

/** @brief Maximum bcrypt salt length in bytes (16 bytes). */
enum { CSILK_BCRYPT_SALT_BYTES = 16 };

/** @brief Length of the ciphertext output after Eksblowfish (23 bytes). */
enum { CSILK_BCRYPT_CIPHER_OUT = 23 };

/** @brief bcrypt hash buffer size (static — never reallocates).
 *
 * Format: `$2a$XX$` (7) + 53 base64 chars = 60 bytes + NUL terminator. */
enum { CSILK_BCRYPT_HASH_LEN = 61 };

/**
 * @brief Hash a plaintext password with bcrypt.
 *
 * Produces a NUL-terminated bcrypt hash string in the `$2a$XX$...` format.
 * The cost factor controls the number of keying rounds (2^cost).
 *
 * @param password   Plaintext password (may contain NUL bytes; use @p len).
 * @param len        Byte length of @p password.  Values > 72 are truncated.
 * @param cost       Cost factor (CSILK_BCRYPT_MIN_COST..CSILK_BCRYPT_MAX_COST).
 *                   Values outside the range are clamped.
 * @param[out] hash  Output buffer of at least CSILK_BCRYPT_HASH_LEN bytes.
 */
void csilk_bcrypt_hash(const char* password, size_t len, int cost, char hash[CSILK_BCRYPT_HASH_LEN]);

/**
 * @brief Verify a plaintext password against a bcrypt hash.
 *
 * Re-hashes @p password with the cost and salt extracted from @p hash,
 * then compares the result using constant-time comparison.
 *
 * @param password  Plaintext password.
 * @param len       Byte length of @p password.
 * @param hash      NUL-terminated bcrypt hash string (must start with `$2`).
 * @return 0 if the password matches, -1 otherwise.
 */
int csilk_bcrypt_verify(const char* password, size_t len, const char* hash);

#ifdef __cplusplus
}
#endif
