/**
 * @file cipher.c
 * @brief Default cipher driver implementation (OpenSSL-backed).
 *
 * Architecture: Provides a concrete implementation of the abstract
 * csilk_cipher_driver_t interface using OpenSSL EVP primitives.
 * This module is the default driver installed by the cipher subsystem.
 * All functions are stateless and thread-safe (OpenSSL is expected to
 * have been configured with locking callbacks at process start).
 *
 * Supported operations:
 * - AES-256-GCM symmetric encrypt/decrypt (authenticated encryption)
 * - RSA-OAEP asymmetric encrypt/decrypt (with SHA-256)
 * - RSA-PSS sign/verify (with SHA-256)
 * - RSA keypair generation (PEM-encoded)
 *
 * @copyright MIT License
 */

#include "csilk/drivers/cipher.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <string.h>

#ifndef RSA_PSS_SALTLEN_DIGEST
#define RSA_PSS_SALTLEN_DIGEST -1
#endif

/** @brief AES-256-GCM symmetric encryption.
 *
 * Algorithm:
 * 1. Create an EVP_CIPHER_CTX for AES-256-GCM.
 * 2. Call EVP_EncryptInit_ex with the key and IV (96-bit nonce).
 * 3. Feed plaintext via EVP_EncryptUpdate to produce ciphertext.
 * 4. Finalize with EVP_EncryptFinal_ex (produces no extra ciphertext
 *    for GCM, but validates internal state).
 * 5. Retrieve the 128-bit authentication tag via EVP_CTRL_GCM_GET_TAG.
 *
 * @param key            256-bit (32-byte) AES key.
 * @param key_len        Must be 32.
 * @param plaintext      Input plaintext.
 * @param plaintext_len  Length of plaintext.
 * @param iv             96-bit (12-byte) initialization vector.
 * @param iv_len         Must be 12.
 * @param ciphertext     [out] Output buffer for encrypted data (must be
 *                       at least plaintext_len + 16 bytes).
 * @param ciphertext_len [out] Receives the ciphertext length.
 * @param tag            [out] 128-bit (16-byte) authentication tag.
 * @param tag_len        Must be 16.
 * @return 0 on success, -1 if parameters are invalid or encryption fails.
 * @note The output buffer must be large enough for ciphertext + tag.
 *       Thread-safe provided OpenSSL is configured with locking. */
static int default_symmetric_encrypt(const uint8_t* key, size_t key_len,
                                     const uint8_t* plaintext,
                                     size_t plaintext_len, const uint8_t* iv,
                                     size_t iv_len, uint8_t* ciphertext,
                                     size_t* ciphertext_len, uint8_t* tag,
                                     size_t tag_len) {
  if (!key || !plaintext || !iv || !ciphertext || !ciphertext_len || !tag)
    return -1;
  if (key_len != 32 || iv_len != 12 || tag_len != 16) return -1;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return -1;

  int ret = -1;
  int len = 0;

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) != 1) goto out;
  if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1)
    goto out;
  *ciphertext_len = (size_t)len;

  if (EVP_EncryptFinal_ex(ctx, ciphertext + *ciphertext_len, &len) != 1)
    goto out;
  *ciphertext_len += (size_t)len;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)tag_len, tag) != 1)
    goto out;

  ret = 0;

out:
  EVP_CIPHER_CTX_free(ctx);
  return ret;
}

/** @brief AES-256-GCM symmetric decryption with authentication tag
 * verification.
 *
 * Algorithm:
 * 1. Create an EVP_CIPHER_CTX for AES-256-GCM.
 * 2. Initialize for decryption with EVP_DecryptInit_ex.
 * 3. Feed ciphertext via EVP_DecryptUpdate to produce plaintext.
 * 4. Set the expected authentication tag via EVP_CTRL_GCM_SET_TAG.
 * 5. Finalize with EVP_DecryptFinal_ex — this fails if the tag does
 *    not match (tampered ciphertext or wrong key).
 *
 * @param key            256-bit AES key (must be 32 bytes).
 * @param key_len        Must be 32.
 * @param ciphertext     Input encrypted data.
 * @param ciphertext_len Length of ciphertext.
 * @param iv             96-bit IV (must be 12 bytes).
 * @param iv_len         Must be 12.
 * @param tag            128-bit authentication tag to verify (16 bytes).
 * @param tag_len        Must be 16.
 * @param plaintext      [out] Output buffer for decrypted data.
 * @param plaintext_len  [out] Receives the plaintext length.
 * @return 0 on success, -1 on authentication failure or invalid params.
 * @note A return of -1 does NOT distinguish between "wrong key",
 *       "tampered ciphertext", or "invalid parameters". */
static int default_symmetric_decrypt(const uint8_t* key, size_t key_len,
                                     const uint8_t* ciphertext,
                                     size_t ciphertext_len, const uint8_t* iv,
                                     size_t iv_len, const uint8_t* tag,
                                     size_t tag_len, uint8_t* plaintext,
                                     size_t* plaintext_len) {
  if (!key || !ciphertext || !iv || !tag || !plaintext || !plaintext_len)
    return -1;
  if (key_len != 32 || iv_len != 12 || tag_len != 16) return -1;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return -1;

  int ret = -1;
  int len = 0;

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) != 1) goto out;
  if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1)
    goto out;
  *plaintext_len = (size_t)len;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tag_len,
                          (void*)tag) != 1)
    goto out;

  if (EVP_DecryptFinal_ex(ctx, plaintext + *plaintext_len, &len) != 1) goto out;
  *plaintext_len += (size_t)len;

  ret = 0;

out:
  EVP_CIPHER_CTX_free(ctx);
  return ret;
}

/** @brief Generate an RSA keypair and return PEM-encoded public and private
 * keys.
 *
 * Algorithm:
 * 1. Create an EVP_PKEY_CTX for RSA key generation.
 * 2. Initialize keygen with CSILK_RSA_KEY_BITS (2048 or 4096).
 * 3. Generate the keypair.
 * 4. Write the public key to a memory BIO in PEM format.
 * 5. Write the private key (unencrypted) to a memory BIO in PEM format.
 * 6. Copy the PEM strings into the caller's buffers.
 *
 * @param public_key  [out] Buffer for PEM-encoded public key.
 * @param pub_len     [in/out] Input: buffer capacity. Output: actual key
 *                    length including null terminator.
 * @param private_key [out] Buffer for PEM-encoded private key.
 * @param priv_len    [in/out] Input: buffer capacity. Output: actual key
 *                    length including null terminator.
 * @return 0 on success, -1 on failure (allocation, keygen, or buffer too
 * small).
 * @note If the output buffers are too small, the function sets *pub_len and
 *       *priv_len to the required sizes and returns -1. */
static int default_generate_keypair(char* public_key, size_t* pub_len,
                                    char* private_key, size_t* priv_len) {
  if (!public_key || !pub_len || !private_key || !priv_len) return -1;

  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  if (!kctx) return -1;

  int ret = -1;
  EVP_PKEY* pkey = NULL;

  if (EVP_PKEY_keygen_init(kctx) != 1) goto out;
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, CSILK_RSA_KEY_BITS) != 1) goto out;
  if (EVP_PKEY_keygen(kctx, &pkey) != 1) goto out;

  BIO* pub_bio = BIO_new(BIO_s_mem());
  BIO* priv_bio = BIO_new(BIO_s_mem());
  if (!pub_bio || !priv_bio) {
    BIO_free(pub_bio);
    BIO_free(priv_bio);
    goto out;
  }

  if (PEM_write_bio_PUBKEY(pub_bio, pkey) != 1) {
    BIO_free(pub_bio);
    BIO_free(priv_bio);
    goto out;
  }
  if (PEM_write_bio_PrivateKey(priv_bio, pkey, NULL, NULL, 0, NULL, NULL) !=
      1) {
    BIO_free(pub_bio);
    BIO_free(priv_bio);
    goto out;
  }

  size_t pub_key_len = (size_t)BIO_pending(pub_bio);
  size_t priv_key_len = (size_t)BIO_pending(priv_bio);

  if (pub_key_len > *pub_len || priv_key_len > *priv_len) {
    BIO_free(pub_bio);
    BIO_free(priv_bio);
    *pub_len = pub_key_len;
    *priv_len = priv_key_len;
    goto out;
  }

  *pub_len = pub_key_len;
  *priv_len = priv_key_len;
  BIO_read(pub_bio, public_key, (int)pub_key_len);
  BIO_read(priv_bio, private_key, (int)priv_key_len);
  public_key[pub_key_len] = '\0';
  private_key[priv_key_len] = '\0';

  BIO_free(pub_bio);
  BIO_free(priv_bio);
  ret = 0;

out:
  EVP_PKEY_free(pkey);
  EVP_PKEY_CTX_free(kctx);
  return ret;
}

/** @brief Internal: parse a PEM-encoded public key into an EVP_PKEY handle.
 * @param pem PEM string buffer.
 * @param len Length of the PEM string.
 * @return New EVP_PKEY with a reference count of 1, or NULL on parse failure.
 * @note The caller must free the returned key with EVP_PKEY_free(). */
static EVP_PKEY* pem_to_pkey(const char* pem, size_t len) {
  BIO* bio = BIO_new_mem_buf(pem, (int)len);
  if (!bio) return NULL;
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return pkey;
}

/** @brief Internal: parse a PEM-encoded private key into an EVP_PKEY handle.
 * @param pem PEM string buffer.
 * @param len Length of the PEM string.
 * @return New EVP_PKEY with a reference count of 1, or NULL on parse failure.
 * @note The caller must free the returned key with EVP_PKEY_free(). */
static EVP_PKEY* pem_to_privkey(const char* pem, size_t len) {
  BIO* bio = BIO_new_mem_buf(pem, (int)len);
  if (!bio) return NULL;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return pkey;
}

/** @brief RSA-OAEP asymmetric encryption.
 *
 * Algorithm:
 * 1. Parse the PEM public key into an EVP_PKEY.
 * 2. Create an EVP_PKEY_CTX for encryption.
 * 3. Configure RSA-OAEP padding with SHA-256 for both the OAEP hash
 *    and the MGF1 mask generation function.
 * 4. Call EVP_PKEY_encrypt to produce the ciphertext.
 *
 * @param public_key     PEM-encoded RSA public key.
 * @param pub_len        Length of public_key string.
 * @param plaintext      Input plaintext (max ~190 bytes for 2048-bit RSA).
 * @param plaintext_len  Length of plaintext.
 * @param ciphertext     [out] Output buffer for encrypted data.
 * @param ciphertext_len [in/out] Input: buffer capacity. Output: ciphertext
 * len.
 * @return 0 on success, -1 on failure.
 * @note The ciphertext length will be equal to the RSA key size (e.g., 256
 *       bytes for a 2048-bit key). */
static int default_asymmetric_encrypt(const char* public_key, size_t pub_len,
                                      const uint8_t* plaintext,
                                      size_t plaintext_len, uint8_t* ciphertext,
                                      size_t* ciphertext_len) {
  if (!public_key || !plaintext || !ciphertext || !ciphertext_len) return -1;

  EVP_PKEY* pkey = pem_to_pkey(public_key, pub_len);
  if (!pkey) return -1;

  int ret = -1;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, NULL);
  if (!ctx) goto out;

  if (EVP_PKEY_encrypt_init(ctx) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) != 1) goto out2;

  size_t out_len = *ciphertext_len;
  if (EVP_PKEY_encrypt(ctx, ciphertext, &out_len, plaintext, plaintext_len) !=
      1)
    goto out2;

  *ciphertext_len = out_len;
  ret = 0;

out2:
  EVP_PKEY_CTX_free(ctx);
out:
  EVP_PKEY_free(pkey);
  return ret;
}

/** @brief RSA-OAEP asymmetric decryption.
 *
 * Algorithm:
 * 1. Parse the PEM private key into an EVP_PKEY.
 * 2. Create an EVP_PKEY_CTX for decryption.
 * 3. Configure RSA-OAEP padding with SHA-256 (matching encrypt).
 * 4. Call EVP_PKEY_decrypt to recover the plaintext.
 *
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Length of private_key string.
 * @param ciphertext     Input ciphertext (must equal RSA key size).
 * @param ciphertext_len Length of ciphertext.
 * @param plaintext      [out] Output buffer for decrypted data.
 * @param plaintext_len  [in/out] Input: buffer capacity. Output: plaintext len.
 * @return 0 on success, -1 on failure (wrong key, tampered data, or params). */
static int default_asymmetric_decrypt(const char* private_key, size_t priv_len,
                                      const uint8_t* ciphertext,
                                      size_t ciphertext_len, uint8_t* plaintext,
                                      size_t* plaintext_len) {
  if (!private_key || !ciphertext || !plaintext || !plaintext_len) return -1;

  EVP_PKEY* pkey = pem_to_privkey(private_key, priv_len);
  if (!pkey) return -1;

  int ret = -1;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, NULL);
  if (!ctx) goto out;

  if (EVP_PKEY_decrypt_init(ctx) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) != 1) goto out2;

  size_t out_len = *plaintext_len;
  if (EVP_PKEY_decrypt(ctx, plaintext, &out_len, ciphertext, ciphertext_len) !=
      1)
    goto out2;

  *plaintext_len = out_len;
  ret = 0;

out2:
  EVP_PKEY_CTX_free(ctx);
out:
  EVP_PKEY_free(pkey);
  return ret;
}

/** @brief RSA-PSS digital signature generation.
 *
 * Algorithm:
 * 1. Parse the PEM private key into an EVP_PKEY.
 * 2. Create an EVP_MD_CTX and initialize for signing with SHA-256.
 * 3. Configure RSA-PSS padding with salt length equal to digest length.
 * 4. Call EVP_DigestSign which internally hashes the data and signs.
 *
 * @param private_key    PEM-encoded RSA private key.
 * @param priv_len       Length of private_key string.
 * @param data           Input data to sign.
 * @param data_len       Length of data.
 * @param signature      [out] Output buffer for the signature.
 * @param sig_len        [in/out] Input: buffer capacity. Output: signature len.
 * @return 0 on success, -1 on failure. */
static int default_sign(const char* private_key, size_t priv_len,
                        const uint8_t* data, size_t data_len,
                        uint8_t* signature, size_t* sig_len) {
  if (!private_key || !data || !signature || !sig_len) return -1;

  EVP_PKEY* pkey = pem_to_privkey(private_key, priv_len);
  if (!pkey) return -1;

  int ret = -1;
  EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
  if (!mdctx) goto out;

  if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1) goto out2;
  if (EVP_PKEY_CTX_set_rsa_padding(EVP_MD_CTX_pkey_ctx(mdctx),
                                   RSA_PKCS1_PSS_PADDING) != 1)
    goto out2;
  if (EVP_PKEY_CTX_set_rsa_pss_saltlen(EVP_MD_CTX_pkey_ctx(mdctx),
                                       RSA_PSS_SALTLEN_DIGEST) != 1)
    goto out2;

  size_t slen = *sig_len;
  if (EVP_DigestSign(mdctx, signature, &slen, data, data_len) != 1) goto out2;

  *sig_len = slen;
  ret = 0;

out2:
  EVP_MD_CTX_free(mdctx);
out:
  EVP_PKEY_free(pkey);
  return ret;
}

/** @brief RSA-PSS digital signature verification.
 *
 * Algorithm:
 * 1. Parse the PEM public key into an EVP_PKEY.
 * 2. Create an EVP_MD_CTX and initialize for verification with SHA-256.
 * 3. Configure RSA-PSS padding with salt length equal to digest length.
 * 4. Call EVP_DigestVerify which internally hashes the data and verifies
 *    the signature.
 *
 * @param public_key  PEM-encoded RSA public key.
 * @param pub_len     Length of public_key string.
 * @param data        Original signed data.
 * @param data_len    Length of data.
 * @param signature   Signature to verify.
 * @param sig_len     Length of signature.
 * @return 0 on valid signature, -1 on invalid signature or error. */
static int default_verify(const char* public_key, size_t pub_len,
                          const uint8_t* data, size_t data_len,
                          const uint8_t* signature, size_t sig_len) {
  if (!public_key || !data || !signature) return -1;

  EVP_PKEY* pkey = pem_to_pkey(public_key, pub_len);
  if (!pkey) return -1;

  int ret = -1;
  EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
  if (!mdctx) goto out;

  if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1)
    goto out2;
  if (EVP_PKEY_CTX_set_rsa_padding(EVP_MD_CTX_pkey_ctx(mdctx),
                                   RSA_PKCS1_PSS_PADDING) != 1)
    goto out2;
  if (EVP_PKEY_CTX_set_rsa_pss_saltlen(EVP_MD_CTX_pkey_ctx(mdctx),
                                       RSA_PSS_SALTLEN_DIGEST) != 1)
    goto out2;

  int v = EVP_DigestVerify(mdctx, signature, sig_len, data, data_len);
  if (v != 1) goto out2;

  ret = 0;

out2:
  EVP_MD_CTX_free(mdctx);
out:
  EVP_PKEY_free(pkey);
  return ret;
}

/** @brief Default cipher driver vtable mapping all operations to the
 *  OpenSSL-backed implementations above.
 *
 * Installed by the cipher subsystem as the default driver. Callers
 * can override individual operations by building a custom driver
 * struct with different function pointers for specific needs (e.g.,
 * hardware-backed keys). */
csilk_cipher_driver_t csilk_default_cipher_driver = {
    .symmetric_encrypt = default_symmetric_encrypt,
    .symmetric_decrypt = default_symmetric_decrypt,
    .generate_keypair = default_generate_keypair,
    .asymmetric_encrypt = default_asymmetric_encrypt,
    .asymmetric_decrypt = default_asymmetric_decrypt,
    .sign = default_sign,
    .verify = default_verify,
};
