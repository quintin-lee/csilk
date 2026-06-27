# Pluggable Crypto Driver Design

csilk allows developers to replace its internal cryptographic and unique identifier algorithms. This is useful for utilizing hardware-accelerated crypto, integrating with system-level libraries, or using localized algorithms (like SM series). Crypto driver **MUST** implement all required fields in `csilk_crypto_driver_t` — partial implementations **MUST** be rejected at registration. Driver lookup **MUST** be O(1) via a fixed-size registry hash table. SHA-256 hashing **SHOULD** complete in ≤ 1µs for inputs ≤ 256 bytes. UUID v4 generation **MUST** use a cryptographically secure random number generator (CSPRNG).

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

The built-in default (`csilk_default_cipher_driver` in `src/crypto/cipher.c`) uses:

- **OpenSSL EVP API** for all algorithms
- **AES-256-GCM** via `EVP_aes_256_gcm()`
- **RSA-2048** keygen via `EVP_PKEY_keygen()`
- **RSA-OAEP** via `EVP_PKEY_encrypt_init()` with `RSA_PKCS1_OAEP_PADDING`
- **RSA-PSS** via `EVP_DigestSign()` with `RSA_PKCS1_PSS_PADDING`

### Internal Delegation Mechanism

Internal wrappers in `crypto_dispatch.h` (included via `internal.h`, implemented in `utils.c`) follow the same pattern as the crypto primitive driver:

```c
int _csilk_symmetric_encrypt(csilk_ctx_t* c, ...) {
  csilk_cipher_driver_t* d = resolve_cipher(c);
  if (d && d->symmetric_encrypt) return d->symmetric_encrypt(...);
  return -1;
}
```

The `resolve_cipher()` helper returns the context's cipher driver if set, otherwise the built-in `csilk_default_cipher_driver`.
