# Pluggable Crypto Driver Design

csilk allows developers to replace its internal cryptographic and unique identifier algorithms. This is useful for utilizing hardware-accelerated crypto, integrating with system-level libraries, or using localized algorithms (like SM series).

## Interface Definition

The crypto driver is a structure of function pointers defined in `csilk.h`:

```c
typedef struct {
  /** @brief Compute SHA256 hash. */
  void (*sha256)(const uint8_t* data, size_t len, uint8_t out[32]);
  /** @brief Compute HMAC-SHA256. */
  void (*hmac_sha256)(const uint8_t* key, size_t key_len, const uint8_t* data,
                       size_t data_len, uint8_t out[32]);
  /** @brief Generate a random UUID v4 string. */
  void (*generate_uuid)(char buf[37]);
} csilk_crypto_driver_t;
```

## Integration Lifecycle

1. **Initialization**: Create a static or allocated instance of `csilk_crypto_driver_t`.
2. **Registration**: Call `csilk_server_set_crypto_driver(server, &my_driver)` before starting the server.
3. **Propagation**: The server automatically attaches the driver to every `csilk_ctx_t`.
4. **Execution**: Middlewares (like JWT and Session) use the driver provided in the context.

## Usage Example

```c
#include <openssl/hmac.h>
#include "csilk.h"

// Example: Using OpenSSL's HMAC implementation
void openssl_hmac(const uint8_t* key, size_t key_len, const uint8_t* data,
                  size_t data_len, uint8_t out[32]) {
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, key_len, data, data_len, out, &len);
}

static csilk_crypto_driver_t openssl_driver = {
    .hmac_sha256 = openssl_hmac,
    .sha256 = NULL,      // Use default built-in implementation
    .generate_uuid = NULL // Use default built-in implementation
};

int main() {
    csilk_server_t* server = csilk_server_new(router);
    
    // Switch to OpenSSL HMAC
    csilk_server_set_crypto_driver(server, &openssl_driver);
    
    csilk_server_run(server, 8080);
}
```

## Internal Mechanism

The framework provides internal wrappers (declared in `csilk_internal.h`) that handle the delegation logic:

```c
void _csilk_hmac_sha256(csilk_ctx_t* c, const uint8_t* key, size_t key_len,
                        const uint8_t* data, size_t data_len, uint8_t out[32]) {
  if (c && c->crypto_driver && c->crypto_driver->hmac_sha256) {
    c->crypto_driver->hmac_sha256(key, key_len, data, data_len, out);
  } else {
    csilk_hmac_sha256(key, key_len, data, data_len, out);
  }
}
```
