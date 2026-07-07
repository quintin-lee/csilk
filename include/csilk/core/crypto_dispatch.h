/**
 * @file crypto_dispatch.h
 * @brief Internal crypto/cipher dispatch stubs and helpers.
 *
 * Weak-symbol stubs that route cryptographic operations through the server's
 * crypto/cipher driver if one is installed, or fall back to built-in
 * software implementations.  Also provides internal I/O and client accessor
 * helpers used by protocol implementations.
 * @copyright MIT License
 */

#ifndef CSILK_CRYPTO_DISPATCH_H
#define CSILK_CRYPTO_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/csilk.h"

/**
 * @brief Internal: Trigger the response send path.
 *
 * Weak symbol so that tests or custom builds can override it.  Called
 * when an async handler finishes and the response should be flushed.
 *
 * @param c  Request context.
 */
CSILK_INTERNAL void _csilk_send_response(csilk_ctx_t* c) __attribute__((weak));

/**
 * @brief Internal: Send data through the appropriate I/O path.
 *
 * Routes data through the TLS wrapper if TLS is enabled, or writes directly
 * to the TCP socket otherwise.
 *
 * @param c    Request context.
 * @param data Bytes to send.
 * @param len  Number of bytes to send.
 */
CSILK_INTERNAL void _csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len);

/**
 * @brief Internal: Compute HMAC-SHA256 using the server's crypto driver (if
 * set) or the built-in software implementation.
 *
 * @param c        Request context (for driver lookup).
 * @param key      HMAC key.
 * @param key_len  Key length.
 * @param data     Input data.
 * @param data_len Input length.
 * @param[out] out 32-byte HMAC output.
 */
CSILK_INTERNAL void _csilk_hmac_sha256(csilk_ctx_t*   c,
                                       const uint8_t* key,
                                       size_t         key_len,
                                       const uint8_t* data,
                                       size_t         data_len,
                                       uint8_t        out[32]);

/**
 * @brief Internal: Generate a random UUID v4 string using the crypto driver
 * (if set) or the built-in /dev/urandom-based implementation.
 *
 * @param c   Request context (for driver lookup).
 * @param[out] buf  Output buffer of at least 37 bytes.  Receives a
 *                  NUL-terminated UUID string (e.g.,
 *                  "f81d4fae-7dec-11d0-a765-00a0c91e6bf6").
 */
CSILK_INTERNAL void _csilk_generate_uuid(csilk_ctx_t* c, char buf[37]);

/**
 * @brief Internal: Fill a buffer with cryptographically secure random bytes
 * using the crypto driver (if set) or the built-in implementation.
 *
 * @param c    Request context (for driver lookup, may be nullptr).
 * @param out  Buffer to fill.
 * @param len  Number of bytes to generate.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_fill_random(csilk_ctx_t* c, void* out, size_t len);

/** @brief Get the internal client connection object.
 *
 * Opaque pointer used by protocol implementations (WebSocket, SSE).
 *
 * @param c  Request context.
 * @return Internal client handle. */
CSILK_INTERNAL void* _csilk_get_internal_client(csilk_ctx_t* c);

/** @brief Set the internal client connection object.
 *
 * @param c       Request context.
 * @param client  Pointer to csilk_client_t (or mock marker). */
CSILK_INTERNAL void _csilk_set_internal_client(csilk_ctx_t* c, void* client);

/**
 * @brief Internal: Symmetric encrypt using the context's cipher driver
 * or the built-in OpenSSL AES-256-GCM implementation.
 *
 * @param c              Request context (for driver lookup, may be nullptr).
 * @param key            Encryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param plaintext      Data to encrypt.
 * @param plaintext_len  Plaintext length.
 * @param iv             12-byte initialisation vector (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param[out] ciphertext  Output buffer (must be at least plaintext_len bytes).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext length.
 * @param[out] tag       16-byte authentication tag buffer.
 * @param tag_len        Tag buffer size (must be 16).
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_symmetric_encrypt(csilk_ctx_t*   c,
                                            const uint8_t* key,
                                            size_t         key_len,
                                            const uint8_t* plaintext,
                                            size_t         plaintext_len,
                                            const uint8_t* iv,
                                            size_t         iv_len,
                                            uint8_t*       ciphertext,
                                            size_t*        ciphertext_len,
                                            uint8_t*       tag,
                                            size_t         tag_len);

/**
 * @brief Internal: Symmetric decrypt using the context's cipher driver
 * or the built-in OpenSSL AES-256-GCM implementation.
 *
 * @param c              Request context (for driver lookup, may be nullptr).
 * @param key            Decryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param ciphertext     Data to decrypt.
 * @param ciphertext_len Ciphertext length.
 * @param iv             12-byte initialisation vector (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param tag            16-byte authentication tag.
 * @param tag_len        Tag length (must be 16).
 * @param[out] plaintext   Output buffer (must be at least ciphertext_len
 * bytes).
 * @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
 * @return 0 on success, -1 on failure (including tag mismatch).
 */
CSILK_INTERNAL int _csilk_symmetric_decrypt(csilk_ctx_t*   c,
                                            const uint8_t* key,
                                            size_t         key_len,
                                            const uint8_t* ciphertext,
                                            size_t         ciphertext_len,
                                            const uint8_t* iv,
                                            size_t         iv_len,
                                            const uint8_t* tag,
                                            size_t         tag_len,
                                            uint8_t*       plaintext,
                                            size_t*        plaintext_len);

/**
 * @brief Internal: Generate an RSA-2048 key pair using the context's cipher
 * driver or the built-in OpenSSL implementation.
 *
 * Keys are output as PEM-encoded strings.
 *
 * @param c            Request context (for driver lookup, may be nullptr).
 * @param[out] public_key   PEM public key buffer.
 * @param[in,out] pub_len   In: capacity, Out: actual PEM length (incl. NUL).
 * @param[out] private_key  PEM private key buffer.
 * @param[in,out] priv_len  In: capacity, Out: actual PEM length (incl. NUL).
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_generate_keypair(
    csilk_ctx_t* c, char* public_key, size_t* pub_len, char* private_key, size_t* priv_len);

/**
 * @brief Internal: Asymmetric encrypt using the context's cipher driver
 * or the built-in OpenSSL RSA-OAEP implementation.
 *
 * @param c              Request context (for driver lookup, may be nullptr).
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Public key length.
 * @param plaintext      Data to encrypt (max ~190 bytes for RSA-2048).
 * @param plaintext_len  Plaintext length.
 * @param[out] ciphertext  256-byte output buffer.
 * @param[in,out] ciphertext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_asymmetric_encrypt(csilk_ctx_t*   c,
                                             const char*    public_key,
                                             size_t         pub_len,
                                             const uint8_t* plaintext,
                                             size_t         plaintext_len,
                                             uint8_t*       ciphertext,
                                             size_t*        ciphertext_len);

/**
 * @brief Internal: Asymmetric decrypt using the context's cipher driver
 * or the built-in OpenSSL RSA-OAEP implementation.
 *
 * @param c              Request context (for driver lookup, may be nullptr).
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Private key length.
 * @param ciphertext     Data to decrypt (typically 256 bytes for RSA-2048).
 * @param ciphertext_len Ciphertext length.
 * @param[out] plaintext   Output buffer.
 * @param[in,out] plaintext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_asymmetric_decrypt(csilk_ctx_t*   c,
                                             const char*    private_key,
                                             size_t         priv_len,
                                             const uint8_t* ciphertext,
                                             size_t         ciphertext_len,
                                             uint8_t*       plaintext,
                                             size_t*        plaintext_len);

/**
 * @brief Internal: Sign data using the context's cipher driver
 * or the built-in OpenSSL RSA-PSS implementation.
 *
 * @param c            Request context (for driver lookup, may be nullptr).
 * @param private_key  PEM-encoded RSA private key.
 * @param priv_len     Private key length.
 * @param data         Data to sign.
 * @param data_len     Data length.
 * @param[out] signature  256-byte signature buffer.
 * @param[in,out] sig_len  In: capacity, Out: actual signature length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int _csilk_sign(csilk_ctx_t*   c,
                               const char*    private_key,
                               size_t         priv_len,
                               const uint8_t* data,
                               size_t         data_len,
                               uint8_t*       signature,
                               size_t*        sig_len);

/**
 * @brief Internal: Verify a signature using the context's cipher driver
 * or the built-in OpenSSL RSA-PSS implementation.
 *
 * @param c           Request context (for driver lookup, may be nullptr).
 * @param public_key  PEM-encoded RSA public key.
 * @param pub_len     Public key length.
 * @param data        Original signed data.
 * @param data_len    Data length.
 * @param signature   Signature to verify.
 * @param sig_len     Signature length.
 * @return 0 on valid signature, -1 on invalid or error.
 */
CSILK_INTERNAL int _csilk_verify(csilk_ctx_t*   c,
                                 const char*    public_key,
                                 size_t         pub_len,
                                 const uint8_t* data,
                                 size_t         data_len,
                                 const uint8_t* signature,
                                 size_t         sig_len);

#include "csilk/core/crypto.h"

CSILK_INTERNAL int _csilk_jwt_sign(csilk_ctx_t*    c,
                                   const char*     key,
                                   size_t          key_len,
                                   const uint8_t*  data,
                                   size_t          data_len,
                                   uint8_t*        signature,
                                   size_t*         sig_len,
                                   csilk_jwt_alg_t algorithm);

CSILK_INTERNAL int _csilk_jwt_verify(csilk_ctx_t*    c,
                                     const char*     key,
                                     size_t          key_len,
                                     const uint8_t*  data,
                                     size_t          data_len,
                                     const uint8_t*  signature,
                                     size_t          sig_len,
                                     csilk_jwt_alg_t algorithm);

/**
 * @brief Generate a random UUID v4 string (standalone, no context needed).
 *
 * Uses /dev/urandom or an equivalent OS entropy source.
 *
 * @param[out] buf  Output buffer of at least 37 bytes.  Populated with a
 *                  NUL-terminated UUID string.
 */
void csilk_generate_uuid(char* buf);

#endif /* CSILK_CRYPTO_DISPATCH_H */
