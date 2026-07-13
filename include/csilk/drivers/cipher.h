#pragma once
/**
 * @file cipher.h
 * @brief Pluggable cryptographic primitive driver interface.
 *
 * Defines the virtual function table (csilk_cipher_driver_t) that abstracts
 * symmetric (AES-256-GCM), asymmetric (RSA-OAEP), signing (RSA-PSS), and
 * key-generation operations.  Implementations may use OpenSSL, BearSSL, or
 * hardware-backed keystores.
 *
 * The driver is set per-server via csilk_server_set_cipher_driver() and
 * propagated to each request context.  Built-in functions in internal.h
 * (e.g., _csilk_symmetric_encrypt) dispatch through this driver when one
 * is installed, falling back to the software implementation otherwise.
 *
 * All functions return 0 on success and -1 on failure.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief AES-256 key length in bytes (32 bytes = 256 bits). */
enum { CSILK_AES256_KEY_SIZE = 32 };
/** @brief GCM initialisation vector (nonce) length in bytes (96 bits). */
enum { CSILK_GCM_IV_SIZE = 12 };
/** @brief GCM authentication tag length in bytes (128 bits). */
enum { CSILK_GCM_TAG_SIZE = 16 };
/** @brief RSA key size in bits for key generation. */
enum { CSILK_RSA_KEY_BITS = 2048 };
/** @brief RSA ciphertext output size in bytes (256 bytes for RSA-2048). */
enum { CSILK_RSA_KEY_SIZE = 256 };
/** @brief RSA signature output size in bytes (256 bytes for RSA-2048). */
enum { CSILK_RSA_SIGNATURE_SIZE = 256 };
/** @brief ECDSA P-256 raw signature size in bytes (32+32 bytes r||s). */
enum { CSILK_ES256_SIGNATURE_SIZE = 64 };

/**
 * @brief Virtual function table implemented by each cipher backend.
 *
 * All function pointers must be non-nullptr except where noted.
 * Operations follow the same parameter patterns as their _csilk_*
 * counterparts in internal.h so that the dispatch layer is transparent.
 */
typedef struct {
    /** @brief AES-256-GCM encryption.
   *  @param key            Encryption key (must be CSILK_AES256_KEY_SIZE
   * bytes).
   *  @param key_len        Key length (must be 32).
   *  @param plaintext      Data to encrypt.
   *  @param plaintext_len  Plaintext length.
   *  @param iv             12-byte nonce (must be CSILK_GCM_IV_SIZE bytes).
   *  @param iv_len         12.
   *  @param[out] ciphertext  Output buffer (>= plaintext_len bytes).
   *  @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext
   * length.
   *  @param[out] tag       16-byte authentication tag buffer.
   *  @param tag_len        16.
   *  @return 0 on success, -1 on failure. */
    int (*symmetric_encrypt)(const uint8_t* key,
                             size_t         key_len,
                             const uint8_t* plaintext,
                             size_t         plaintext_len,
                             const uint8_t* iv,
                             size_t         iv_len,
                             uint8_t*       ciphertext,
                             size_t*        ciphertext_len,
                             uint8_t*       tag,
                             size_t         tag_len);

    /** @brief AES-256-GCM decryption with authentication tag verification.
   *  @param key            Decryption key.
   *  @param key_len        Key length.
   *  @param ciphertext     Data to decrypt.
   *  @param ciphertext_len Ciphertext length.
   *  @param iv             12-byte nonce.
   *  @param iv_len         12.
   *  @param tag            16-byte authentication tag.
   *  @param tag_len        16.
   *  @param[out] plaintext   Output buffer (>= ciphertext_len bytes).
   *  @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
   *  @return 0 on success, -1 on tag mismatch or error. */
    int (*symmetric_decrypt)(const uint8_t* key,
                             size_t         key_len,
                             const uint8_t* ciphertext,
                             size_t         ciphertext_len,
                             const uint8_t* iv,
                             size_t         iv_len,
                             const uint8_t* tag,
                             size_t         tag_len,
                             uint8_t*       plaintext,
                             size_t*        plaintext_len);

    /** @brief Generate an RSA-2048 key pair.
   *  @param[out] public_key   PEM-encoded public key output buffer.
   *  @param[in,out] pub_len   In: capacity, Out: actual PEM length.
   *  @param[out] private_key  PEM-encoded private key output buffer.
   *  @param[in,out] priv_len  In: capacity, Out: actual PEM length.
   *  @return 0 on success, -1 on failure. */
    int (*generate_keypair)(char* public_key, size_t* pub_len, char* private_key, size_t* priv_len);

    /** @brief RSA-OAEP encryption.
   *  @param public_key     PEM-encoded RSA public key.
   *  @param pub_len        Public key PEM length.
   *  @param plaintext      Data to encrypt (max ~190 bytes for RSA-2048).
   *  @param plaintext_len  Plaintext length.
   *  @param[out] ciphertext  Output buffer (>= CSILK_RSA_KEY_SIZE bytes).
   *  @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext
   * length.
   *  @return 0 on success, -1 on failure. */
    int (*asymmetric_encrypt)(const char*    public_key,
                              size_t         pub_len,
                              const uint8_t* plaintext,
                              size_t         plaintext_len,
                              uint8_t*       ciphertext,
                              size_t*        ciphertext_len);

    /** @brief RSA-OAEP decryption.
   *  @param private_key    PEM-encoded RSA private key.
   *  @param priv_len       Private key PEM length.
   *  @param ciphertext     Data to decrypt.
   *  @param ciphertext_len Ciphertext length (typically 256).
   *  @param[out] plaintext   Output buffer.
   *  @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
   *  @return 0 on success, -1 on failure. */
    int (*asymmetric_decrypt)(const char*    private_key,
                              size_t         priv_len,
                              const uint8_t* ciphertext,
                              size_t         ciphertext_len,
                              uint8_t*       plaintext,
                              size_t*        plaintext_len);

    /** @brief RSA-PSS signature generation.
   *  @param private_key    PEM-encoded RSA private key.
   *  @param priv_len       Private key PEM length.
   *  @param data           Data to sign.
   *  @param data_len       Data length.
   *  @param[out] signature   Output buffer (>= CSILK_RSA_SIGNATURE_SIZE bytes).
   *  @param[in,out] sig_len  In: capacity, Out: actual signature length.
   *  @return 0 on success, -1 on failure. */
    int (*sign)(const char*    private_key,
                size_t         priv_len,
                const uint8_t* data,
                size_t         data_len,
                uint8_t*       signature,
                size_t*        sig_len);

    /** @brief RSA-PSS signature verification.
   *  @param public_key     PEM-encoded RSA public key.
   *  @param pub_len        Public key PEM length.
   *  @param data           Original signed data.
   *  @param data_len       Data length.
   *  @param signature      Signature to verify.
   *  @param sig_len        Signature length.
   *  @return 0 on valid signature, -1 on invalid or error. */
    int (*verify)(const char*    public_key,
                  size_t         pub_len,
                  const uint8_t* data,
                  size_t         data_len,
                  const uint8_t* signature,
                  size_t         sig_len);

    /** @brief JWT signing — supports HS256, RS256, ES256.
     *  For HS256, key is the raw secret string and sig_len is 32.
     *  For RS256, key is a PEM-encoded RSA private key.
     *  For ES256, key is a PEM-encoded EC private key.
     *  @param key         Signing key.
     *  @param key_len     Key length in bytes.
     *  @param data        Data to sign (the JWT signing input).
     *  @param data_len    Data length.
     *  @param[out] signature  Output buffer.
     *  @param[in,out] sig_len  In: capacity, Out: actual signature length.
     *  @param algorithm   JWT algorithm identifier.
     *  @return 0 on success, -1 on failure. */
    int (*jwt_sign)(const char*     key,
                    size_t          key_len,
                    const uint8_t*  data,
                    size_t          data_len,
                    uint8_t*        signature,
                    size_t*         sig_len,
                    csilk_jwt_alg_t algorithm);

    /** @brief JWT signature verification.
     *  @param key         Verification key.
     *  @param key_len     Key length.
     *  @param data        Original signed data.
     *  @param data_len    Data length.
     *  @param signature   Signature to verify.
     *  @param sig_len     Signature length.
     *  @param algorithm   JWT algorithm identifier.
     *  @return 0 on valid signature, -1 on invalid or error. */
    int (*jwt_verify)(const char*     key,
                      size_t          key_len,
                      const uint8_t*  data,
                      size_t          data_len,
                      const uint8_t*  signature,
                      size_t          sig_len,
                      csilk_jwt_alg_t algorithm);
} csilk_cipher_driver_t;

#ifdef __cplusplus
}
#endif
