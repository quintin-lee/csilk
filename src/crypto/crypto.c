/**
 * @file crypto.c
 * @brief Cryptographic primitives: SHA-256, HMAC, UUID, random, nonce.
 *
 * Implements:
 *   - SHA-256 : HMAC, session integrity
 *   - HMAC-SHA256 : Keyed-hash message authentication
 *   - UUID generation (RFC 4122)
 *   - Platform RNG (getrandom/arc4random/CryptGenRandom/urandom)
 *   - Monotonic nonce fallback for GCM safety
 *
 * Cipher dispatch (AES/RSA/JWT) moved to cipher_dispatch.c.
 * @copyright MIT License
 */

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

#include "core/ctx/ctx_internal.h"
#include <stdlib.h>
#include "csilk/core/internal.h"
#include "csilk/crypto/crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

static _Atomic uint32_t g_nonce_counter = 0;

/**
 * @brief Initialize a SHA-256 hashing context using OpenSSL.
 * @param context SHA-256 context to initialize (must not be NULL).
 */
void
csilk_sha256_init(csilk_sha256_ctx* context)
{
    if (context) {
        _Static_assert(sizeof(csilk_sha256_ctx) >= sizeof(SHA256_CTX),
                       "csilk_sha256_ctx too small");
        SHA256_Init((SHA256_CTX*)context);
    }
}

/**
 * @brief Feed data into the SHA-256 hashing context using OpenSSL.
 * @param context SHA-256 context.
 * @param data    Input data buffer.
 * @param len     Length of input data in bytes.
 */
void
csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data, size_t len)
{
    if (context && data && len > 0) {
        SHA256_Update((SHA256_CTX*)context, data, len);
    }
}

/**
 * @brief Finalize the SHA-256 hash and produce the 32-byte digest using OpenSSL.
 * @param context SHA-256 context.
 * @param digest  [out] 32-byte buffer to receive the hash digest.
 */
void
csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32])
{
    if (context && digest) {
        SHA256_Final(digest, (SHA256_CTX*)context);
    }
}

/**
 * @brief Compute HMAC-SHA256 using OpenSSL EVP/HMAC.
 * @param key      HMAC secret key.
 * @param key_len  Key length in bytes.
 * @param data     Input message data.
 * @param data_len Message length in bytes.
 * @param out      [out] 32-byte output buffer for the HMAC digest.
 */
void
csilk_hmac_sha256(
    const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32])
{
    if (!out) {
        return;
    }
    const uint8_t* safe_key = key ? key : (const uint8_t*)"";
    const uint8_t* safe_data = data ? data : (const uint8_t*)"";
    unsigned int   md_len = 32;
    HMAC(EVP_sha256(), safe_key, (int)key_len, safe_data, data_len, out, &md_len);
}

/** @brief Context-aware HMAC-SHA256 — delegates to the crypto driver if
 * available.
 *
 * This is the "late-bound" version of csilk_hmac_sha256(). It checks whether
 * the request context has a crypto driver installed (e.g., OpenSSL, mbedTLS,
 * or a hardware security module). If so, the driver's accelerated HMAC is
 * used. Otherwise, the built-in software implementation serves as the
 * portable fallback.
 *
 * This pattern allows the application to use pluggable crypto backends
 * without changing caller code. The default built-in implementation is
 * always available for environments without hardware crypto.
 *
 * @param c        Request context (may be NULL — falls back to built-in).
 * @param key      HMAC key.
 * @param key_len  Key length.
 * @param data     Input data.
 * @param data_len Data length.
 * @param out      [out] 32-byte HMAC output buffer. */
CSILK_INTERNAL void
_csilk_hmac_sha256(csilk_ctx_t*   c,
                   const uint8_t* key,
                   size_t         key_len,
                   const uint8_t* data,
                   size_t         data_len,
                   uint8_t        out[32])
{
    if (c && c->crypto_driver && c->crypto_driver->hmac_sha256) {
        c->crypto_driver->hmac_sha256(key, key_len, data, data_len, out);
    } else {
        csilk_hmac_sha256(key, key_len, data, data_len, out);
    }
}

/** @brief Context-aware UUID generation — delegates to the crypto driver if
 * available.
 *
 * This is the late-bound UUID generator. If the context has a crypto driver
 * with a cryptographically secure generate_uuid method (e.g., reading from
 * a hardware RNG or via OpenSSL), that is used. Otherwise falls back to the
 * built-in csilk_generate_uuid() which reads /dev/urandom.
 *
 * The delegation pattern ensures callers always get the best available
 * randomness source without explicit driver management.
 *
 * @param c   Request context (may be NULL — falls back to built-in).
 * @param buf [out] 37-byte buffer for the UUID string. */
CSILK_INTERNAL void
_csilk_generate_uuid(csilk_ctx_t* c, char buf[CSILK_UUID_BUF_SIZE])
{
    if (c && c->crypto_driver && c->crypto_driver->generate_uuid) {
        c->crypto_driver->generate_uuid(buf);
    } else {
        csilk_generate_uuid(buf);
    }
}

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 *
 * Delegates to the context's crypto driver when one is installed, otherwise
 * uses the best available platform source (getrandom(2) on Linux,
 * arc4random_buf(3) on BSD/macOS, CryptGenRandom on Windows, or
 * /dev/urandom as a final fallback).
 *
 * @param c   Request context (may be NULL — uses platform RNG).
 * @param out [out] Buffer to fill with random bytes.
 * @param len Number of random bytes to write.
 * @return 0 on success, or -1 if no entropy source was available.
 */
CSILK_INTERNAL int
_csilk_fill_random(csilk_ctx_t* c, void* out, size_t len)
{
    if (c && c->crypto_driver && c->crypto_driver->fill_random) {
        return c->crypto_driver->fill_random(out, len);
    }

#if defined(_WIN32)
    HCRYPTPROV hProvider;
    if (CryptAcquireContext(&hProvider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptGenRandom(hProvider, (DWORD)len, (BYTE*)out)) {
            CryptReleaseContext(hProvider, 0);
            return 0;
        }
        CryptReleaseContext(hProvider, 0);
    }
    return -1;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, len);
    return 0;
#elif defined(__linux__)
    /* Try getrandom() first on Linux (modern, no FD needed) */
    ssize_t ret = getrandom(out, len, 0);
    if (ret == (ssize_t)len) {
        return 0;
    }
#endif

#ifndef _WIN32
    /* Fallback to /dev/urandom for older Linux or other POSIX systems */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t n = fread(out, 1, len, f);
        fclose(f);
        return (n == len) ? 0 : -1;
    }
#endif

    return -1;
}

/**
 * @brief Fill a buffer with cryptographically secure random bytes (no context).
 *
 * Convenience wrapper around _csilk_fill_random() that always uses the
 * platform entropy source (no crypto-driver dispatch).  Suitable for callers
 * that do not have a request context available.
 *
 * @param out [out] Buffer to fill with random bytes.
 * @param len Number of random bytes to write.
 * @return 0 on success, or -1 if the entropy source failed.
 */
int
csilk_crypto_fill_random(void* out, size_t len)
{
    return _csilk_fill_random(NULL, out, len);
}

/**
 * @brief Context-aware SHA-1 digest dispatcher.
 *
 * Delegates to the context's crypto driver when one is installed, otherwise
 * falls back to the built-in software SHA-1 implementation (csilk_sha1_*).
 *
 * @param c   Request context (may be NULL — uses built-in SHA-1).
 * @param data Input message bytes.
 * @param len Length of the input in bytes.
 * @param out [out] 20-byte buffer to receive the SHA-1 digest.
 */
CSILK_INTERNAL void
_csilk_sha1(csilk_ctx_t* c, const uint8_t* data, size_t len, uint8_t out[20])
{
    if (c && c->crypto_driver && c->crypto_driver->sha1) {
        c->crypto_driver->sha1(data, len, out);
    } else {
        csilk_sha1_ctx ctx;
        csilk_sha1_init(&ctx);
        csilk_sha1_update(&ctx, data, len);
        csilk_sha1_final(&ctx, out);
    }
}

/**
 * @brief Context-aware bcrypt password hashing dispatcher.
 *
 * Delegates to the context's crypto driver when one is installed, otherwise
 * falls back to the built-in bcrypt implementation (csilk_bcrypt_hash).
 *
 * @param c       Request context (may be NULL — uses built-in bcrypt).
 * @param password Null-terminated password bytes (truncated to 72 bytes).
 * @param len     Length of the password in bytes.
 * @param cost    bcrypt work factor (10-31); clamped if out of range.
 * @param hash    [out] Buffer of CSILK_BCRYPT_HASH_LEN bytes for the result.
 */
CSILK_INTERNAL void
_csilk_bcrypt_hash(
    csilk_ctx_t* c, const char* password, size_t len, int cost, char hash[CSILK_BCRYPT_HASH_LEN])
{
    if (c && c->crypto_driver && c->crypto_driver->bcrypt_hash) {
        c->crypto_driver->bcrypt_hash(password, len, cost, hash);
    } else {
        csilk_bcrypt_hash(password, len, cost, hash);
    }
}

/**
 * @brief Generate a random nonce, with a monotonic fallback.
 *
 * Fills @p out with @p len cryptographically random bytes.  If the system
 * entropy source fails, falls back to a deterministic-but-unique value built
 * from csilk_io_hrtime() plus an atomic counter, so the nonce is never
 * (accidentally) reused — important for AEAD / nonce-misuse resistance.
 *
 * @param out [out] Buffer to receive the nonce.
 * @param len Number of nonce bytes to generate.
 */
void
csilk_crypto_generate_nonce(uint8_t* out, size_t len)
{
    if (csilk_crypto_fill_random(out, len) != 0) {
        /* Monotonic unique fallback to ensure GCM safety (never reuse nonce)
         * if the system entropy source fails. Uses csilk_io_hrtime() and atomic counter. */
        uint64_t ts = csilk_io_hrtime();
        uint32_t count = atomic_fetch_add(&g_nonce_counter, 1);
        size_t   i = 0;
        for (; i < len && i < 8; i++) {
            out[i] = (uint8_t)(ts >> (i * 8));
        }
        for (; i < len; i++) {
            size_t shift = ((i - 8) % 4) * 8;
            out[i] = (uint8_t)(count >> shift);
        }
    }
}

/**
 * @brief Allocate memory via the standard allocator.
 *
 * Thin convenience wrapper around malloc().  Provided so callers throughout
 * the framework use a single allocation entry point.
 *
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL on failure.
 */
void*
csilk_malloc(size_t size)
{
    return malloc(size);
}

/**
 * @brief Free memory previously allocated by csilk_malloc()/csilk_strdup().
 *
 * Thin convenience wrapper around free().
 *
 * @param ptr Pointer to free (may be NULL).
 */
void
csilk_free(void* ptr)
{
    free(ptr);
}

/**
 * @brief Duplicate a NUL-terminated string.
 *
 * Thin convenience wrapper around strdup() that is NULL-safe: a NULL input
 * yields a NULL return rather than dereferencing NULL.
 *
 * @param s NUL-terminated string to duplicate (may be NULL).
 * @return Heap-allocated copy of @p s, or NULL on allocation failure / NULL input.
 */
char*
csilk_strdup(const char* s)
{
    return s ? strdup(s) : NULL;
}
