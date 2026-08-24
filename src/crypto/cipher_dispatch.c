/**
 * @file cipher_dispatch.c
 * @brief Cipher operation dispatch layer.
 *
 * Implements context-aware dispatch for symmetric encryption,
 * asymmetric encryption, signing, and JWT operations via the
 * csilk_cipher_driver_t VTable.  Falls back to the built-in
 * OpenSSL implementation when no driver is set.
 *
 * @copyright MIT License
 */

#include "core/ctx/ctx_internal.h"
#include <stdlib.h>
#include "csilk/core/internal.h"
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>

extern csilk_cipher_driver_t csilk_default_cipher_driver;

/** @brief Resolve the active cipher driver for a given context.
 *
 * Returns the cipher driver attached to the context, or falls back to the
 * default built-in driver when no context or no driver is set.
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
 * symmetric_encrypt callback.
 *
 * @param c              Request context (may be NULL).
 * @param key            Encryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param plaintext      Data to encrypt.
 * @param plaintext_len  Plaintext length.
 * @param iv             12-byte IV (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param[out] ciphertext  Output buffer (must be ≥ plaintext_len).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual length.
 * @param[out] tag       16-byte authentication tag buffer.
 * @param tag_len        Tag buffer size (must be 16).
 * @return 0 on success, -1 on failure.
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
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * symmetric_decrypt callback.
 *
 * @param c              Request context (may be NULL).
 * @param key            Decryption key (must be 32 bytes for AES-256).
 * @param key_len        Key length.
 * @param ciphertext     Data to decrypt.
 * @param ciphertext_len Ciphertext length.
 * @param iv             12-byte IV (nonce).
 * @param iv_len         IV length (must be 12 for GCM).
 * @param tag            16-byte authentication tag.
 * @param tag_len        Tag length (must be 16).
 * @param[out] plaintext   Output buffer (≥ ciphertext_len).
 * @param[in,out] plaintext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure (including tag mismatch).
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

/** @brief RSA keypair generation dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * generate_keypair callback.
 *
 * @param c            Request context (may be NULL).
 * @param[out] public_key   PEM public key buffer.
 * @param[in,out] pub_len   In: capacity, Out: actual PEM length (incl. NUL).
 * @param[out] private_key  PEM private key buffer.
 * @param[in,out] priv_len  In: capacity, Out: actual PEM length (incl. NUL).
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int
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
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * asymmetric_encrypt callback.
 *
 * @param c              Request context (may be NULL).
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Public key length.
 * @param plaintext      Data to encrypt (max ~190 bytes for RSA-2048).
 * @param plaintext_len  Plaintext length.
 * @param[out] ciphertext  256-byte output buffer.
 * @param[in,out] ciphertext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int
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
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * asymmetric_decrypt callback.
 *
 * @param c              Request context (may be NULL).
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Private key length.
 * @param ciphertext     Data to decrypt (typically 256 bytes for RSA-2048).
 * @param ciphertext_len Ciphertext length.
 * @param[out] plaintext   Output buffer.
 * @param[in,out] plaintext_len  In: capacity, Out: actual length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int
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

/** @brief Signing dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * sign callback.
 *
 * @param c            Request context (may be NULL).
 * @param private_key  PEM-encoded RSA private key.
 * @param priv_len     Private key length.
 * @param data         Data to sign.
 * @param data_len     Data length.
 * @param[out] signature  256-byte signature buffer.
 * @param[in,out] sig_len  In: capacity, Out: actual signature length.
 * @return 0 on success, -1 on failure.
 */
CSILK_INTERNAL int
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

/** @brief Signature verification dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * verify callback.
 *
 * @param c           Request context (may be NULL).
 * @param public_key  PEM-encoded RSA public key.
 * @param pub_len     Public key length.
 * @param data        Original signed data.
 * @param data_len    Data length.
 * @param signature   Signature to verify.
 * @param sig_len     Signature length.
 * @return 0 on valid signature, -1 on invalid or error.
 */
CSILK_INTERNAL int
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

/** @brief JWT signing dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * jwt_sign callback.
 *
 * @param c         Request context (may be NULL).
 * @param key       Signing key.
 * @param key_len   Key length.
 * @param data      Data to sign.
 * @param data_len  Data length.
 * @param[out] signature  Output signature buffer.
 * @param[in,out] sig_len  In: capacity, Out: actual signature length.
 * @param algorithm JWT algorithm selector.
 * @return 0 on success, -1 on failure.
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

/** @brief JWT verification dispatcher.
 *
 * Resolves the cipher driver via resolve_cipher() and delegates to its
 * jwt_verify callback.
 *
 * @param c           Request context (may be NULL).
 * @param key         Verification key.
 * @param key_len     Key length.
 * @param data        Signed data.
 * @param data_len    Data length.
 * @param signature   Signature to verify.
 * @param sig_len     Signature length.
 * @param algorithm   JWT algorithm selector.
 * @return 0 on valid signature, -1 on invalid or error.
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

/* ============================================================================
 * Public API — standalone cipher operations (no request context required)
 * ============================================================================ */

/**
 * @brief AES-256-GCM symmetric encryption.
 *
 * Validates parameters then delegates to the built-in cipher driver.
 */
int
csilk_symmetric_encrypt(const uint8_t* key,
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
    if (!key || !plaintext || !iv || !ciphertext || !ciphertext_len || !tag) {
        return -1;
    }
    if (key_len != CSILK_AES256_KEY_SIZE || iv_len != CSILK_GCM_IV_SIZE ||
        tag_len != CSILK_GCM_TAG_SIZE) {
        return -1;
    }
    return _csilk_symmetric_encrypt(NULL,
                                    key,
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

/**
 * @brief AES-256-GCM symmetric decryption with tag verification.
 */
int
csilk_symmetric_decrypt(const uint8_t* key,
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
    if (!key || !ciphertext || !iv || !tag || !plaintext || !plaintext_len) {
        return -1;
    }
    if (key_len != CSILK_AES256_KEY_SIZE || iv_len != CSILK_GCM_IV_SIZE ||
        tag_len != CSILK_GCM_TAG_SIZE) {
        return -1;
    }
    return _csilk_symmetric_decrypt(NULL,
                                    key,
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

/**
 * @brief Generate an RSA-2048 key pair (PEM-encoded).
 */
int
csilk_rsa_generate_keypair(char* public_key, size_t* pub_len, char* private_key, size_t* priv_len)
{
    if (!pub_len || !priv_len) {
        return -1;
    }
    return _csilk_generate_keypair(NULL, public_key, pub_len, private_key, priv_len);
}

/**
 * @brief RSA-OAEP encryption.
 */
int
csilk_rsa_encrypt(const char*    public_key,
                  size_t         pub_len,
                  const uint8_t* plaintext,
                  size_t         plaintext_len,
                  uint8_t*       ciphertext,
                  size_t*        ciphertext_len)
{
    if (!public_key || !plaintext || !ciphertext || !ciphertext_len) {
        return -1;
    }
    return _csilk_asymmetric_encrypt(
        NULL, public_key, pub_len, plaintext, plaintext_len, ciphertext, ciphertext_len);
}

/**
 * @brief RSA-OAEP decryption.
 */
int
csilk_rsa_decrypt(const char*    private_key,
                  size_t         priv_len,
                  const uint8_t* ciphertext,
                  size_t         ciphertext_len,
                  uint8_t*       plaintext,
                  size_t*        plaintext_len)
{
    if (!private_key || !ciphertext || !plaintext || !plaintext_len) {
        return -1;
    }
    return _csilk_asymmetric_decrypt(
        NULL, private_key, priv_len, ciphertext, ciphertext_len, plaintext, plaintext_len);
}

/**
 * @brief RSA-PSS signature generation.
 */
int
csilk_rsa_sign(const char*    private_key,
               size_t         priv_len,
               const uint8_t* data,
               size_t         data_len,
               uint8_t*       signature,
               size_t*        sig_len)
{
    if (!private_key || !data || !signature || !sig_len) {
        return -1;
    }
    return _csilk_sign(NULL, private_key, priv_len, data, data_len, signature, sig_len);
}

/**
 * @brief RSA-PSS signature verification.
 */
int
csilk_rsa_verify(const char*    public_key,
                 size_t         pub_len,
                 const uint8_t* data,
                 size_t         data_len,
                 const uint8_t* signature,
                 size_t         sig_len)
{
    if (!public_key || !data || !signature) {
        return -1;
    }
    return _csilk_verify(NULL, public_key, pub_len, data, data_len, signature, sig_len);
}
