#ifndef CSILK_CIPHER_H
#define CSILK_CIPHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSILK_AES256_KEY_SIZE 32
#define CSILK_GCM_IV_SIZE 12
#define CSILK_GCM_TAG_SIZE 16
#define CSILK_RSA_KEY_BITS 2048
#define CSILK_RSA_KEY_SIZE 256
#define CSILK_RSA_SIGNATURE_SIZE 256

typedef struct {
  int (*symmetric_encrypt)(const uint8_t* key, size_t key_len,
                           const uint8_t* plaintext, size_t plaintext_len,
                           const uint8_t* iv, size_t iv_len,
                           uint8_t* ciphertext, size_t* ciphertext_len,
                           uint8_t* tag, size_t tag_len);

  int (*symmetric_decrypt)(const uint8_t* key, size_t key_len,
                           const uint8_t* ciphertext, size_t ciphertext_len,
                           const uint8_t* iv, size_t iv_len, const uint8_t* tag,
                           size_t tag_len, uint8_t* plaintext,
                           size_t* plaintext_len);

  int (*generate_keypair)(char* public_key, size_t* pub_len, char* private_key,
                          size_t* priv_len);

  int (*asymmetric_encrypt)(const char* public_key, size_t pub_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            uint8_t* ciphertext, size_t* ciphertext_len);

  int (*asymmetric_decrypt)(const char* private_key, size_t priv_len,
                            const uint8_t* ciphertext, size_t ciphertext_len,
                            uint8_t* plaintext, size_t* plaintext_len);

  int (*sign)(const char* private_key, size_t priv_len, const uint8_t* data,
              size_t data_len, uint8_t* signature, size_t* sig_len);

  int (*verify)(const char* public_key, size_t pub_len, const uint8_t* data,
                size_t data_len, const uint8_t* signature, size_t sig_len);
} csilk_cipher_driver_t;

#ifdef __cplusplus
}
#endif

#endif
