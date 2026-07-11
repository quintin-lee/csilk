/**
 * @file utils.c
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

#include "../ctx/ctx_internal.h"
#include <stdlib.h>
#include "csilk/core/internal.h"
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"

static _Atomic uint32_t g_nonce_counter = 0;

#ifdef TEST_OOM
int g_oom_fail_after = -1;
int g_oom_count = 0;
#endif

/* --- SHA256 Implementation --- */

/* 32-bit rotate-right (circular shift) — inverse of SHA-1's rol. */
#define ror(value, bits) (((value) >> (bits)) | ((value) << (32 - (bits))))

/* SHA-256 Choose function: Ch(x,y,z) = (x & y) ^ (~x & z)
 * For each bit position, selects y when x=1, z when x=0. */
#define ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))

/* SHA-256 Majority function: Maj(x,y,z) = (x&y) ^ (x&z) ^ (y&z)
 * Returns 1 when at least two of the three bits are 1. */
#define maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* SHA-256 uppercase Sigma functions (compression function rounds):
 * Sigma0(x) = ROTR^2(x) XOR ROTR^13(x) XOR ROTR^22(x) — used in T2.
 * Sigma1(x) = ROTR^6(x) XOR ROTR^11(x) XOR ROTR^25(x) — used in T1. */
#define sigma0(x) (ror(x, 2) ^ ror(x, 13) ^ ror(x, 22))
#define sigma1(x) (ror(x, 6) ^ ror(x, 11) ^ ror(x, 25))

/* SHA-256 lowercase sigma functions (message schedule expansion):
 * gamma0(x) = ROTR^7(x) XOR ROTR^18(x) XOR SHR^3(x) — for w[i-15].
 * gamma1(x) = ROTR^17(x) XOR ROTR^19(x) XOR SHR^10(x) — for w[i-2]. */
#define gamma0(x) (ror(x, 7) ^ ror(x, 18) ^ ((x) >> 3))
#define gamma1(x) (ror(x, 17) ^ ror(x, 19) ^ ((x) >> 10))

static const uint32_t k256[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/** @brief Process a single 64-byte block through the SHA-256 compression
 * function (FIPS 180-4 §6.2.2).
 *
 * The SHA-256 compression function operates on a 256-bit (8-word) state and
 * a 512-bit (64-byte) message block:
 *
 *   1. Message Schedule (w[0..63]): The first 16 words are the message block
 *      in big-endian. Words 16-63 are expanded using:
 *        w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16]
 *      where gamma0 and gamma1 are the "lowercase sigma" diffusion functions.
 *
 *   2. State initialisation: a..h = state[0..7].
 *
 *   3. Compression loop (64 rounds): Each round computes:
 *        T1 = h + Sigma1(e) + Ch(e,f,g) + K[i] + w[i]
 *        T2 = Sigma0(a) + Maj(a,b,c)
 *        a..h = (T1+T2, a, b, c, d+T1, e, f, g)
 *      The K constants are the first 32 bits of the fractional parts of the
 *      cube roots of the first 64 primes (nothing-up-my-sleeve numbers).
 *
 *   4. State update: state[n] += working variable (a..h).
 *
 * @param state [in/out] 8-element hash state (updated in-place).
 * @param data  64-byte (512-bit) message block to process. */
static void
sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

    /* Message schedule: first 16 words = big-endian message block */
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16 |
               (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
    }
    /* Message schedule expansion (w[16..63]) using the lowercase sigma
   * functions for diffusion across the entire message. */
    for (int i = 16; i < 64; i++) {
        w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    /* 64 rounds of the SHA-256 compression function.
   * Each round mixes the working variables through Sigma, Ch, Maj, and
   * the round constant K[i] for both diffusion and Confusion. */
    for (int i = 0; i < 64; i++) {
        t1 = h + sigma1(e) + ch(e, f, g) + k256[i] + w[i];
        t2 = sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/** @brief Initialize a SHA-256 hashing context with the standard initial hash
 * values.
 *
 * Sets the eight state words to the SHA-256 initial constants and resets
 * the bit count to zero.
 *
 * @param context SHA-256 context to initialize (must not be nullptr). */
void
csilk_sha256_init(csilk_sha256_ctx* context)
{
    context->state[0] = 0x6a09e667;
    context->state[1] = 0xbb67ae85;
    context->state[2] = 0x3c6ef372;
    context->state[3] = 0xa54ff53a;
    context->state[4] = 0x510e527f;
    context->state[5] = 0x9b05688c;
    context->state[6] = 0x1f83d9ab;
    context->state[7] = 0x5be0cd19;
    context->count = 0;
}

/** @brief Feed data into the SHA-256 hashing context for incremental hashing.
 *
 * Processes the input data in 64-byte blocks, updating the context's state.
 * Partial blocks are buffered. Tracks the total bit count for final padding.
 *
 * @param context SHA-256 context (initialized via csilk_sha256_init()).
 * @param data    Input data buffer.
 * @param len     Length of input data in bytes. */
void
csilk_sha256_update(csilk_sha256_ctx* context, const uint8_t* data, size_t len)
{
    uint32_t i, idx = (uint32_t)((context->count >> 3) & 0x3F);
    context->count += (uint64_t)len << 3;

    if (64 - idx <= len) {
        memcpy(context->buffer + idx, data, 64 - idx);
        sha256_transform(context->state, context->buffer);
        for (i = 64 - idx; i + 63 < len; i += 64) {
            sha256_transform(context->state, data + i);
        }
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(context->buffer + idx, data + i, len - i);
}

/** @brief Finalize the SHA-256 hash and produce the 32-byte digest.
 *
 * Pads the message according to FIPS 180-4, appends the 64-bit message
 * length, and outputs the final 256-bit (32-byte) hash digest.
 *
 * @param context SHA-256 context with accumulated data.
 * @param digest  [out] 32-byte buffer to receive the hash digest. */
void
csilk_sha256_final(csilk_sha256_ctx* context, uint8_t digest[32])
{
    uint8_t finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((context->count >> (56 - i * 8)) & 0xFF);
    }

    uint32_t left = (uint32_t)((context->count >> 3) % 64);
    uint32_t pad_len = (left < 56) ? (56 - left) : (120 - left);
    uint8_t  padding[128] = {0x80};
    csilk_sha256_update(context, padding, pad_len);
    csilk_sha256_update(context, finalcount, 8);

    for (int i = 0; i < 32; i++) {
        digest[i] = (uint8_t)((context->state[i >> 2] >> (24 - (i & 3) * 8)) & 0xFF);
    }
}

/** @brief Compute HMAC-SHA256 as defined in RFC 2104.
 *
 * HMAC (Hash-based Message Authentication Code) provides both data integrity
 * and authenticity via a shared secret. The construction is:
 *
 *   HMAC(K, m) = SHA256((K' XOR opad) || SHA256((K' XOR ipad) || m))
 *
 * where:
 *   - K' is the key, hashed with SHA256 if longer than 64 bytes (block size).
 *   - ipad = 0x36 repeated 64 times (inner padding).
 *   - opad = 0x5C repeated 64 times (outer padding).
 *   - || denotes concatenation.
 *
 * The double-hashing protects against length-extension attacks on the
 * underlying hash function. The ipad/opad XOR ensures that the inner and
 * outer hashes use distinct keys derived from the same secret.
 *
 * @param key      HMAC secret key.
 * @param key_len  Key length in bytes.
 * @param data     Input message data.
 * @param data_len Message length in bytes.
 * @param out      [out] 32-byte output buffer for the HMAC digest. */
void
csilk_hmac_sha256(
    const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32])
{
    csilk_sha256_ctx ctx;
    uint8_t          k_ipad[64], k_opad[64], tk[32];

    if (key_len > 64) {
        csilk_sha256_init(&ctx);
        csilk_sha256_update(&ctx, key, key_len);
        csilk_sha256_final(&ctx, tk);
        key = tk;
        key_len = 32;
    }

    memset(k_ipad, 0, 64);
    memset(k_opad, 0, 64);
    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, k_ipad, 64);
    csilk_sha256_update(&ctx, data, data_len);
    csilk_sha256_final(&ctx, out);

    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, k_opad, 64);
    csilk_sha256_update(&ctx, out, 32);
    csilk_sha256_final(&ctx, out);
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
 * @param c        Request context (may be nullptr — falls back to built-in).
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
 * @param c   Request context (may be nullptr — falls back to built-in).
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

int
csilk_crypto_fill_random(void* out, size_t len)
{
    return _csilk_fill_random(nullptr, out, len);
}

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
 * @param c Server context, may be nullptr.
 * @return Pointer to an active csilk_cipher_driver_t (never nullptr on its own).
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

void*
csilk_malloc(size_t size)
{
    return malloc(size);
}

void
csilk_free(void* ptr)
{
    free(ptr);
}

char*
csilk_strdup(const char* s)
{
    return s ? strdup(s) : nullptr;
}

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
