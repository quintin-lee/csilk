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

static EVP_PKEY* pem_to_pkey(const char* pem, size_t len) {
  BIO* bio = BIO_new_mem_buf(pem, (int)len);
  if (!bio) return NULL;
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return pkey;
}

static EVP_PKEY* pem_to_privkey(const char* pem, size_t len) {
  BIO* bio = BIO_new_mem_buf(pem, (int)len);
  if (!bio) return NULL;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return pkey;
}

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

csilk_cipher_driver_t csilk_default_cipher_driver = {
    .symmetric_encrypt = default_symmetric_encrypt,
    .symmetric_decrypt = default_symmetric_decrypt,
    .generate_keypair = default_generate_keypair,
    .asymmetric_encrypt = default_asymmetric_encrypt,
    .asymmetric_decrypt = default_asymmetric_decrypt,
    .sign = default_sign,
    .verify = default_verify,
};
