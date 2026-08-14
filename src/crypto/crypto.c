/**
 * @file crypto.c
 * @brief Cryptographic digests, HMAC, and symmetric/asymmetric crypto dispatch.
 *
 * Implements:
 *   - SHA-256 : HMAC, JWT signing, session integrity — full FIPS 180-4 impl.
 *   - HMAC-SHA256 : Keyed-hash message authentication (RFC 2104) for JWT, CSRF.
 *   - Context-aware crypto dispatchers for encryption, signing, key generation.
 *
 * SHA-1 has been moved to sha1.c, Base64/Base64URL to base64.c, UUID to uuid.c.
 * All functions support the internal dispatch pattern: they can be called
 * standalone (using built-in software implementations) or delegating through
 * the context's crypto/cipher driver when one is set.
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
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"

static _Atomic uint32_t g_nonce_counter = 0;

#ifdef TEST_OOM
_Atomic int g_oom_fail_after = -1;
_Atomic int g_oom_count = 0;
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

/**
 * @brief Initialize a SHA-256 hashing context using OpenSSL.
 * @param context SHA-256 context to initialize (must not be NULL).
 */
void
csilk_sha256_init(csilk_sha256_ctx* context)
{
    if (context) {
        SHA256_Init(context);
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
        SHA256_Update(context, data, len);
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
        SHA256_Final(digest, context);
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
            out[i] = (uint8_t)(count >> ((i - 8) * 8));
        }
    }
}

extern csilk_cipher_driver_t csilk_default_cipher_driver;

/** @brief Resolve the active cipher driver for a given context.
 *
 * Returns the cipher driver attached to the context, or falls back to the
 * default built-in driver when no context or no driver is set.  This is the
 * central dispatch helper used by all _csilk_* crypto wrappers.
 *
 * @param c Server context, may be NULL.
 * @return Pointer to an active csilk_cipher_driver_t (never NULL on its own).
 * @note The fallback driver is declared as a weak symbol so that
 *       applications can override it at link time.
 */
static csilk_cipher_driver_t*
resolve_cipher(csilk_ctx_t* c)
{
    if (c && c->cipher_driver) {
        return c->cipher_driver;
    }
    return &csilk_default_cipher_driver;
}

/** @brief Symmetric encryption dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * symmetric_encrypt callback.  Useful for AEAD ciphers where the tag (e.g.
 * GCM authentication tag) is written separately.
 *
 * @param c Server context (driver resolution).
 * @param key Symmetric key.
 * @param key_len Length of key in bytes.
 * @param plaintext Input plaintext.
 * @param plaintext_len Length of plaintext.
 * @param iv Initialisation vector / nonce.
 * @param iv_len Length of IV.
 * @param[out] ciphertext Output buffer for ciphertext.
 * @param[in,out] ciphertext_len On input, capacity of ciphertext buffer; on
 *                 output, bytes written.
 * @param[out] tag Output buffer for authentication tag.
 * @param tag_len Requested tag length in bytes.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
CSILK_INTERNAL int
_csilk_symmetric_encrypt(csilk_ctx_t*   c,
                         const uint8_t* key,
                         size_t         key_len,
                         const uint8_t* plaintext,
                         size_t         plaintext_len,
                         const uint8_t* iv,
                         size_t         iv_len,
                         uint8_t*       ciphertext,
                         size_t*        ciphertext_len,
                         uint8_t*       tag,
                         size_t         tag_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->symmetric_encrypt) {
        return -1;
    }
    return d->symmetric_encrypt(key,
                                key_len,
                                plaintext,
                                plaintext_len,
                                iv,
                                iv_len,
                                ciphertext,
                                ciphertext_len,
                                tag,
                                tag_len);
}

/** @brief Symmetric decryption dispatcher.
 *
 * Resolves the cipher driver and delegates to its symmetric_decrypt
 * callback.  Performs AEAD decryption — the caller must supply the
 * authentication tag produced during encryption.
 *
 * @param c Server context (driver resolution).
 * @param key Symmetric key.
 * @param key_len Length of key in bytes.
 * @param ciphertext Input ciphertext.
 * @param ciphertext_len Length of ciphertext.
 * @param iv Initialisation vector / nonce used during encryption.
 * @param iv_len Length of IV.
 * @param tag Authentication tag to verify.
 * @param tag_len Length of the tag.
 * @param[out] plaintext Output buffer for decrypted data.
 * @param[in,out] plaintext_len On input, capacity of plaintext buffer; on
 *                 output, bytes written.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
CSILK_INTERNAL int
_csilk_symmetric_decrypt(csilk_ctx_t*   c,
                         const uint8_t* key,
                         size_t         key_len,
                         const uint8_t* ciphertext,
                         size_t         ciphertext_len,
                         const uint8_t* iv,
                         size_t         iv_len,
                         const uint8_t* tag,
                         size_t         tag_len,
                         uint8_t*       plaintext,
                         size_t*        plaintext_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->symmetric_decrypt) {
        return -1;
    }
    return d->symmetric_decrypt(key,
                                key_len,
                                ciphertext,
                                ciphertext_len,
                                iv,
                                iv_len,
                                tag,
                                tag_len,
                                plaintext,
                                plaintext_len);
}

/** @brief Asymmetric key-pair generation dispatcher.
 *
 * Resolves the cipher driver and delegates to its generate_keypair
 * callback.  The generated keys are returned as PEM-encoded strings.
 *
 * @param c Server context (driver resolution).
 * @param[out] public_key Buffer for the PEM-encoded public key.
 * @param[in,out] pub_len On input, capacity of public_key buffer; on
 *                output, bytes written (including NUL terminator).
 * @param[out] private_key Buffer for the PEM-encoded private key.
 * @param[in,out] priv_len On input, capacity of private_key buffer; on
 *                 output, bytes written (including NUL terminator).
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
int
_csilk_generate_keypair(
    csilk_ctx_t* c, char* public_key, size_t* pub_len, char* private_key, size_t* priv_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->generate_keypair) {
        return -1;
    }
    return d->generate_keypair(public_key, pub_len, private_key, priv_len);
}

/** @brief Asymmetric encryption dispatcher.
 *
 * Resolves the cipher driver and delegates to its asymmetric_encrypt
 * callback.  Typically used with RSA or ECIES-style encryption schemes.
 *
 * @param c Server context (driver resolution).
 * @param public_key PEM-encoded public key of the recipient.
 * @param pub_len Length of the public key string (including NUL).
 * @param plaintext Input plaintext.
 * @param plaintext_len Length of plaintext.
 * @param[out] ciphertext Output buffer for encrypted data.
 * @param[in,out] ciphertext_len On input, capacity of ciphertext buffer; on
 *                 output, bytes written.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
int
_csilk_asymmetric_encrypt(csilk_ctx_t*   c,
                          const char*    public_key,
                          size_t         pub_len,
                          const uint8_t* plaintext,
                          size_t         plaintext_len,
                          uint8_t*       ciphertext,
                          size_t*        ciphertext_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->asymmetric_encrypt) {
        return -1;
    }
    return d->asymmetric_encrypt(
        public_key, pub_len, plaintext, plaintext_len, ciphertext, ciphertext_len);
}

/** @brief Asymmetric decryption dispatcher.
 *
 * Resolves the cipher driver and delegates to its asymmetric_decrypt
 * callback.  Decrypts data that was previously encrypted with the
 * corresponding public key.
 *
 * @param c Server context (driver resolution).
 * @param private_key PEM-encoded private key of the recipient.
 * @param priv_len Length of the private key string (including NUL).
 * @param ciphertext Input ciphertext.
 * @param ciphertext_len Length of ciphertext.
 * @param[out] plaintext Output buffer for decrypted data.
 * @param[in,out] plaintext_len On input, capacity of plaintext buffer; on
 *                 output, bytes written.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
int
_csilk_asymmetric_decrypt(csilk_ctx_t*   c,
                          const char*    private_key,
                          size_t         priv_len,
                          const uint8_t* ciphertext,
                          size_t         ciphertext_len,
                          uint8_t*       plaintext,
                          size_t*        plaintext_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->asymmetric_decrypt) {
        return -1;
    }
    return d->asymmetric_decrypt(
        private_key, priv_len, ciphertext, ciphertext_len, plaintext, plaintext_len);
}

/** @brief Digital signature creation dispatcher.
 *
 * Resolves the cipher driver and delegates to its sign callback.  Creates a
 * digital signature over the supplied data using the private key.
 *
 * @param c Server context (driver resolution).
 * @param private_key PEM-encoded private key used for signing.
 * @param priv_len Length of the private key string (including NUL).
 * @param data Input data to sign.
 * @param data_len Length of the input data.
 * @param[out] signature Output buffer for the raw signature bytes.
 * @param[in,out] sig_len On input, capacity of the signature buffer; on
 *                output, bytes written.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
int
_csilk_sign(csilk_ctx_t*   c,
            const char*    private_key,
            size_t         priv_len,
            const uint8_t* data,
            size_t         data_len,
            uint8_t*       signature,
            size_t*        sig_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->sign) {
        return -1;
    }
    return d->sign(private_key, priv_len, data, data_len, signature, sig_len);
}

/** @brief Digital signature verification dispatcher.
 *
 * Resolves the cipher driver and delegates to its verify callback.  Checks
 * that the signature is valid for the given data and public key.
 *
 * @param c Server context (driver resolution).
 * @param public_key PEM-encoded public key of the signer.
 * @param pub_len Length of the public key string (including NUL).
 * @param data Data that was signed.
 * @param data_len Length of the signed data.
 * @param signature Raw signature bytes to verify.
 * @param sig_len Length of the signature.
 * @return 0 on success, or a negative error code.
 * @note Falls back to the default cipher driver when the context has no
 *       driver set.
 */
int
_csilk_verify(csilk_ctx_t*   c,
              const char*    public_key,
              size_t         pub_len,
              const uint8_t* data,
              size_t         data_len,
              const uint8_t* signature,
              size_t         sig_len)
{
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->verify) {
        return -1;
    }
    return d->verify(public_key, pub_len, data, data_len, signature, sig_len);
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

/**
 * @brief Context-aware JWT signing dispatcher.
 *
 * Resolves the cipher driver and delegates to its jwt_sign callback to
 * produce a signature over @p data.  When no driver is installed the call
 * fails with -1 (there is no built-in JWT signing fallback).
 *
 * @param c         Request context (driver resolution).
 * @param key       Signing key.
 * @param key_len   Length of the key in bytes.
 * @param data      Data to sign.
 * @param data_len  Length of the data in bytes.
 * @param[out] signature Output buffer for the raw signature.
 * @param[in,out] sig_len On input, capacity of the signature buffer; on
 *                 output, bytes written.
 * @param algorithm JWT algorithm selector (e.g. HS256).
 * @return 0 on success, or -1 on failure.
 */
CSILK_INTERNAL int
_csilk_jwt_sign(csilk_ctx_t*    c,
                const char*     key,
                size_t          key_len,
                const uint8_t*  data,
                size_t          data_len,
                uint8_t*        signature,
                size_t*         sig_len,
                csilk_jwt_alg_t algorithm)
{
    (void)c;
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->jwt_sign) {
        return -1;
    }
    return d->jwt_sign(key, key_len, data, data_len, signature, sig_len, algorithm);
}

/**
 * @brief Context-aware JWT verification dispatcher.
 *
 * Resolves the cipher driver and delegates to its jwt_verify callback to
 * check @p signature against @p data.  When no driver is installed the call
 * fails with -1 (there is no built-in JWT verification fallback).
 *
 * @param c         Request context (driver resolution).
 * @param key       Verification key.
 * @param key_len   Length of the key in bytes.
 * @param data      Signed data.
 * @param data_len  Length of the data in bytes.
 * @param signature Raw signature bytes to verify.
 * @param sig_len   Length of the signature in bytes.
 * @param algorithm JWT algorithm selector (e.g. HS256).
 * @return 0 on success, or -1 if the signature is invalid / unavailable.
 */
CSILK_INTERNAL int
_csilk_jwt_verify(csilk_ctx_t*    c,
                  const char*     key,
                  size_t          key_len,
                  const uint8_t*  data,
                  size_t          data_len,
                  const uint8_t*  signature,
                  size_t          sig_len,
                  csilk_jwt_alg_t algorithm)
{
    (void)c;
    csilk_cipher_driver_t* d = resolve_cipher(c);
    if (!d || !d->jwt_verify) {
        return -1;
    }
    return d->jwt_verify(key, key_len, data, data_len, signature, sig_len, algorithm);
}
