# Pluggable Crypto Driver & Primitives Design

csilk delegates standard cryptographic primitives (SHA-256, HMAC-SHA256, SHA-1, AES-256-GCM, RSA, ECDSA, and OpenSSL-backed bcrypt) directly to OpenSSL, eliminating hand-rolled crypto implementations while automatically benefiting from audited security, side-channel protections, and hardware acceleration (e.g., Intel SHA-NI/AES-NI and ARMv8 Crypto Extensions).

### bcrypt Architecture
- **Cryptographic Randomness**: Employs `RAND_bytes()` / `RAND_priv_bytes()` from `<openssl/rand.h>` for salt generation.
- **Side-Channel Protection**: Performs constant-time hash verification via `CRYPTO_memcmp()`.
- **Zeroization**: Utilizes `OPENSSL_cleanse()` to wipe passwords, salts, and expanded cipher key structures from the stack.
- **Thread Safety**: Operates on a stack-allocated, re-entrant `csilk_bcrypt_state_t` (P-array and S-boxes), eliminating shared mutable state and data races.



The crypto subsystem has two independent pluggable interfaces:

| Interface | Purpose | Header |
|-----------|---------|--------|
| `csilk_crypto_driver_t` | Hash, HMAC, UUID primitives | `csilk.h` |
| `csilk_cipher_driver_t` | Symmetric/asymmetric encryption, signing | `csilk/drivers/cipher.h` |

---

## 1. Crypto Primitive Driver (`csilk_crypto_driver_t`)

### Interface Definition

Defined in `csilk.h`:

```c
typedef struct {
  void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
  void (*hmac_sha256)(const uint8_t* key, size_t key_len, const uint8_t* data,
                        size_t data_len, uint8_t out[32]);
  void (*generate_uuid)(char buf[37]);
  int  (*fill_random)(void* out, size_t len);
  void (*sha1)(const uint8_t* data, size_t len, uint8_t out[20]);
  void (*bcrypt_hash)(const char* password, size_t len, int cost,
                      char hash[62]);
} csilk_crypto_driver_t;
```

### Integration Lifecycle

1. **Initialization**: Create a static or allocated instance of `csilk_crypto_driver_t`.
2. **Registration**: Call `csilk_server_set_crypto_driver(server, &my_driver)` before starting the server.
3. **Propagation**: The server automatically attaches the driver to every `csilk_ctx_t`.
4. **Execution**: Middlewares (like JWT and Session) use the driver provided in the context.

### Usage Example

```c
#include <openssl/hmac.h>
#include "csilk/csilk.h"

void openssl_hmac(const uint8_t* key, size_t key_len, const uint8_t* data,
                  size_t data_len, uint8_t out[32]) {
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, out, &len);
}

static csilk_crypto_driver_t openssl_driver = {
    .hmac_sha256 = openssl_hmac,
};

int main() {
    csilk_server_t* server = csilk_server_new(router);
    csilk_server_set_crypto_driver(server, &openssl_driver);
    csilk_server_run(server, 8080);
}
```

---

## 2. Cipher Driver (`csilk_cipher_driver_t`)

### Interface Definition

Defined in `csilk/drivers/cipher.h`:

```c
#define CSILK_AES256_KEY_SIZE   32
#define CSILK_GCM_IV_SIZE       12
#define CSILK_GCM_TAG_SIZE      16
#define CSILK_RSA_KEY_SIZE      256
#define CSILK_RSA_SIGNATURE_SIZE 256

typedef struct {
  int (*symmetric_encrypt)(const uint8_t* key, size_t key_len,
                           const uint8_t* plaintext, size_t plaintext_len,
                           const uint8_t* iv, size_t iv_len,
                           uint8_t* ciphertext, size_t* ciphertext_len,
                           uint8_t* tag, size_t tag_len);
  int (*symmetric_decrypt)(const uint8_t* key, size_t key_len,
                           const uint8_t* ciphertext, size_t ciphertext_len,
                           const uint8_t* iv, size_t iv_len,
                           const uint8_t* tag, size_t tag_len,
                           uint8_t* plaintext, size_t* plaintext_len);
  int (*generate_keypair)(char* public_key, size_t* pub_len,
                          char* private_key, size_t* priv_len);
  int (*asymmetric_encrypt)(const char* public_key, size_t pub_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            uint8_t* ciphertext, size_t* ciphertext_len);
  int (*asymmetric_decrypt)(const char* private_key, size_t priv_len,
                            const uint8_t* ciphertext, size_t ciphertext_len,
                            uint8_t* plaintext, size_t* plaintext_len);
  int (*sign)(const char* private_key, size_t priv_len,
              const uint8_t* data, size_t data_len,
              uint8_t* signature, size_t* sig_len);
  int (*verify)(const char* public_key, size_t pub_len,
                const uint8_t* data, size_t data_len,
                const uint8_t* signature, size_t sig_len);
} csilk_cipher_driver_t;
```

### Algorithms

| Operation | Algorithm | Parameters |
|-----------|-----------|------------|
| **Symmetric encrypt/decrypt** | AES-256-GCM | 32-byte key, 12-byte IV, 16-byte tag |
| **Key generation** | RSA-2048 | Outputs PEM-encoded key pair |
| **Asymmetric encrypt/decrypt** | RSA-OAEP (SHA-256 MGF1) | Max plaintext ~190 bytes |
| **Sign/verify** | RSA-PSS (SHA-256) | 256-byte signature |

### Integration Lifecycle

Same as the crypto primitive driver:

1. **Initialization**: Create a `csilk_cipher_driver_t` instance with function pointers populated.
2. **Registration**: Call `csilk_server_set_cipher_driver(server, &my_driver)`. Pass NULL to restore defaults.
3. **Propagation**: The server copies the driver to every `csilk_ctx_t` on connection accept.
4. **Execution**: Use the internal wrappers (`_csilk_symmetric_encrypt`, etc.) which delegate to the driver or fall back to defaults.

### Usage Example

```c
#include "csilk/csilk.h"
#include "csilk/core/internal.h"  // umbrella → includes crypto_dispatch.h

void my_handler(csilk_ctx_t* c) {
  uint8_t key[32], iv[12];
  memset(key, 0x2A, 32);
  memset(iv, 0x3B, 12);
  const uint8_t pt[] = "secret data";
  uint8_t ct[64], tag[16], dec[64];
  size_t ct_len = sizeof(ct), dec_len = sizeof(dec);

  // Encrypt (uses default AES-256-GCM or custom driver if set)
  _csilk_symmetric_encrypt(c, key, sizeof(key), pt, strlen((char*)pt),
                           iv, sizeof(iv), ct, &ct_len, tag, sizeof(tag));

  // Decrypt
  _csilk_symmetric_decrypt(c, key, sizeof(key), ct, ct_len,
                           iv, sizeof(iv), tag, sizeof(tag), dec, &dec_len);
}
```

### Custom Driver Example

```c
static int my_aes_encrypt(const uint8_t* key, size_t key_len,
                          const uint8_t* pt, size_t pt_len,
                          const uint8_t* iv, size_t iv_len,
                          uint8_t* ct, size_t* ct_len,
                          uint8_t* tag, size_t tag_len) {
  // Custom AES-256-GCM implementation (hardware-accelerated, etc.)
  return my_hw_aes_gcm_encrypt(key, key_len, pt, pt_len, iv, iv_len, ct, ct_len, tag);
}

static csilk_cipher_driver_t hw_driver = {
    .symmetric_encrypt = my_aes_encrypt,
    .symmetric_decrypt = my_aes_decrypt,
    // NULL entries fall back to default OpenSSL implementation
};

int main() {
    csilk_server_set_cipher_driver(server, &hw_driver);
}
```

### Default Implementation

The built-in default (`csilk_default_cipher_driver` in `src/drivers/cipher/openssl.c`) uses:

- **OpenSSL EVP API** for all algorithms
- **AES-256-GCM** via `EVP_aes_256_gcm()`
- **RSA-2048** keygen via `EVP_PKEY_keygen()`
- **RSA-OAEP** via `EVP_PKEY_encrypt_init()` with `RSA_PKCS1_OAEP_PADDING`
- **RSA-PSS** via `EVP_DigestSign()` with `RSA_PKCS1_PSS_PADDING`

### Internal Delegation Mechanism

Internal wrappers in `crypto_dispatch.h` (included via `internal.h`, implemented in `utils.c`) follow the same pattern as the crypto primitive driver:

```c
int _csilk_symmetric_encrypt(csilk_ctx_t* c, ...) {
  csilk_cipher_driver_t* d = csilk_ctx_get_cipher_driver(c);
  if (d->symmetric_encrypt) return d->symmetric_encrypt(...);
  return -1;
}
```

`csilk_ctx_get_cipher_driver()` returns the cipher driver bound to the context, falling back to `csilk_default_cipher_driver` when none is set.
