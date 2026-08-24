#pragma once
/**
 * @file cipher.h
 * @brief Public cipher API — symmetric (AES-256-GCM) and asymmetric (RSA) operations.
 *
 * Provides standalone cryptographic functions that do not require a request
 * context.  These always use the built-in OpenSSL driver; pluggable drivers
 * via csilk_ctx_set_cipher_driver() are available only through the internal
 * _csilk_* dispatch path for in-request usage.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AES-256-GCM symmetric encryption.
 *
 * @param key            32-byte AES key.
 * @param key_len        Must equal CSILK_AES256_KEY_SIZE (32).
 * @param plaintext      Data to encrypt.
 * @param plaintext_len  Plaintext length.
 * @param iv             12-byte nonce (IV).
 * @param iv_len         Must equal CSILK_GCM_IV_SIZE (12).
 * @param[out] ciphertext  Output buffer (≥ plaintext_len bytes).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext length.
 * @param[out] tag       16-byte authentication tag buffer.
 * @param tag_len        Must equal CSILK_GCM_TAG_SIZE (16).
 * @return 0 on success, -1 on invalid parameters or encryption failure.
 */
int csilk_symmetric_encrypt(const uint8_t* key,
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
 * @brief AES-256-GCM symmetric decryption with tag verification.
 *
 * @param key            32-byte AES key.
 * @param key_len        Must equal CSILK_AES256_KEY_SIZE (32).
 * @param ciphertext     Data to decrypt.
 * @param ciphertext_len Ciphertext length.
 * @param iv             12-byte nonce (must match encryption IV).
 * @param iv_len         Must equal CSILK_GCM_IV_SIZE (12).
 * @param tag            16-byte authentication tag.
 * @param tag_len        Must equal CSILK_GCM_TAG_SIZE (16).
 * @param[out] plaintext   Output buffer (≥ ciphertext_len bytes).
 * @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
 * @return 0 on success, -1 on tag mismatch or decryption failure.
 */
int csilk_symmetric_decrypt(const uint8_t* key,
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
 * @brief Generate an RSA-2048 key pair.
 *
 * Keys are output as PEM-encoded strings.  Callers must allocate buffers
 * large enough to hold the PEM output (typical RSA-2048 private key ≤ 1700
 * bytes, public key ≤ 450 bytes).
 *
 * @param[out] public_key   PEM public key buffer.
 * @param[in,out] pub_len   In: capacity, Out: actual PEM length (incl. NUL).
 * @param[out] private_key  PEM private key buffer.
 * @param[in,out] priv_len  In: capacity, Out: actual PEM length (incl. NUL).
 * @return 0 on success, -1 on failure.
 */
int
csilk_rsa_generate_keypair(char* public_key, size_t* pub_len, char* private_key, size_t* priv_len);

/**
 * @brief RSA-OAEP encryption.
 *
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Public key PEM string length.
 * @param plaintext      Data to encrypt (max ~190 bytes for RSA-2048).
 * @param plaintext_len  Plaintext length.
 * @param[out] ciphertext  Output buffer (≥ CSILK_RSA_KEY_SIZE = 256 bytes).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual ciphertext length.
 * @return 0 on success, -1 on failure.
 */
int csilk_rsa_encrypt(const char*    public_key,
                      size_t         pub_len,
                      const uint8_t* plaintext,
                      size_t         plaintext_len,
                      uint8_t*       ciphertext,
                      size_t*        ciphertext_len);

/**
 * @brief RSA-OAEP decryption.
 *
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Private key PEM string length.
 * @param ciphertext     Data to decrypt (typically 256 bytes for RSA-2048).
 * @param ciphertext_len Ciphertext length.
 * @param[out] plaintext   Output buffer.
 * @param[in,out] plaintext_len  In: capacity, Out: actual plaintext length.
 * @return 0 on success, -1 on failure.
 */
int csilk_rsa_decrypt(const char*    private_key,
                      size_t         priv_len,
                      const uint8_t* ciphertext,
                      size_t         ciphertext_len,
                      uint8_t*       plaintext,
                      size_t*        plaintext_len);

/**
 * @brief RSA-PSS signature generation.
 *
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Private key PEM string length.
 * @param data           Data to sign.
 * @param data_len       Data length.
 * @param[out] signature  Output buffer (≥ CSILK_RSA_SIGNATURE_SIZE = 256 bytes).
 * @param[in,out] sig_len  In: capacity, Out: actual signature length.
 * @return 0 on success, -1 on failure.
 */
int csilk_rsa_sign(const char*    private_key,
                   size_t         priv_len,
                   const uint8_t* data,
                   size_t         data_len,
                   uint8_t*       signature,
                   size_t*        sig_len);

/**
 * @brief RSA-PSS signature verification.
 *
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Public key PEM string length.
 * @param data           Original signed data.
 * @param data_len       Data length.
 * @param signature      Signature to verify.
 * @param sig_len        Signature length.
 * @return 0 on valid signature, -1 on invalid or error.
 */
int csilk_rsa_verify(const char*    public_key,
                     size_t         pub_len,
                     const uint8_t* data,
                     size_t         data_len,
                     const uint8_t* signature,
                     size_t         sig_len);

#ifdef __cplusplus
}
#endif
