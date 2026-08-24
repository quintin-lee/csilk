# Cipher Public API Design

**Date:** 2026-08-24
**Status:** Draft
**Author:** Agnes (Claude)

## Problem

`csilk_cipher_driver_t` exists as an internal dispatch VTable, but no public API wraps it. External consumers must either link against OpenSSL directly or build custom cipher drivers — neither is practical for typical usage.

The `_csilk_*` cipher dispatch functions exist but are marked `CSILK_INTERNAL` and require a `csilk_ctx_t*` context that external code rarely has.

## Goal

Expose a clean, context-free public API for the most common cipher operations, matching the existing pattern of `csilk_sha256_*` / `csilk_hmac_sha256` / `csilk_bcrypt_hash`.

## Proposed API

### New Header: `include/csilk/core/cipher.h`

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "csilk/drivers/cipher.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Symmetric (AES-256-GCM) */
int csilk_symmetric_encrypt(const uint8_t* key, size_t key_len,
                            const uint8_t* plaintext, size_t plaintext_len,
                            const uint8_t* iv, size_t iv_len,
                            uint8_t* ciphertext, size_t* ciphertext_len,
                            uint8_t* tag, size_t tag_len);

int csilk_symmetric_decrypt(const uint8_t* key, size_t key_len,
                            const uint8_t* ciphertext, size_t ciphertext_len,
                            const uint8_t* iv, size_t iv_len,
                            const uint8_t* tag, size_t tag_len,
                            uint8_t* plaintext, size_t* plaintext_len);

/* RSA keypair generation */
int csilk_rsa_generate_keypair(char* public_key, size_t* pub_len,
                               char* private_key, size_t* priv_len);

/* RSA asymmetric encrypt/decrypt */
int csilk_rsa_encrypt(const char* public_key, size_t pub_len,
                      const uint8_t* plaintext, size_t plaintext_len,
                      uint8_t* ciphertext, size_t* ciphertext_len);

int csilk_rsa_decrypt(const char* private_key, size_t priv_len,
                      const uint8_t* ciphertext, size_t ciphertext_len,
                      uint8_t* plaintext, size_t* plaintext_len);

/* RSA signing / verification */
int csilk_rsa_sign(const char* private_key, size_t priv_len,
                   const uint8_t* data, size_t data_len,
                   uint8_t* signature, size_t* sig_len);

int csilk_rsa_verify(const char* public_key, size_t pub_len,
                     const uint8_t* data, size_t data_len,
                     const uint8_t* signature, size_t sig_len);

#ifdef __cplusplus
}
#endif
```

## Implementation Strategy

### File: `src/crypto/cipher_dispatch.c`

Add six public wrapper functions after the existing `_csilk_*` dispatchers. Each wrapper calls its corresponding `_csilk_*` dispatcher with `NULL` context — `resolve_cipher(NULL)` falls back to `&csilk_default_cipher_driver`, which is the built-in OpenSSL implementation.

The wrappers also perform parameter validation (null checks, size checks for AES-256-GCM: key_len==32, iv_len==12, tag_len==16).

```c
// After existing _csilk_* functions in cipher_dispatch.c:

/** @brief AES-256-GCM symmetric encryption (public, uses built-in driver).
 *
 * This is the public entry point for symmetric encryption.
 * It bypasses context-based driver dispatch and always uses the
 * built-in OpenSSL AES-256-GCM implementation.
 *
 * @param key            32-byte AES-256 key.
 * @param key_len        Must be 32.
 * @param plaintext      Data to encrypt.
 * @param plaintext_len  Plaintext length.
 * @param iv             12-byte nonce (GCM IV).
 * @param iv_len         Must be 12.
 * @param[out] ciphertext  Output buffer (≥ plaintext_len bytes).
 * @param[in,out] ciphertext_len  In: capacity, Out: actual length.
 * @param[out] tag       16-byte authentication tag.
 * @param tag_len        Must be 16.
 * @return 0 on success, -1 on invalid parameters or encryption failure.
 */
int
csilk_symmetric_encrypt(const uint8_t* key, size_t key_len,
                        const uint8_t* plaintext, size_t plaintext_len,
                        const uint8_t* iv, size_t iv_len,
                        uint8_t* ciphertext, size_t* ciphertext_len,
                        uint8_t* tag, size_t tag_len)
{
    if (!key || !plaintext || !iv || !ciphertext || !ciphertext_len || !tag) {
        return -1;
    }
    if (key_len != CSILK_AES256_KEY_SIZE || iv_len != CSILK_GCM_IV_SIZE || tag_len != CSILK_GCM_TAG_SIZE) {
        return -1;
    }
    return _csilk_symmetric_encrypt(NULL, key, key_len, plaintext, plaintext_len,
                                     iv, iv_len, ciphertext, ciphertext_len, tag, tag_len);
}

// ... similar for csilk_symmetric_decrypt, csilk_rsa_*, etc.
```

### Why NULL context?

`resolve_cipher(NULL)` returns `&csilk_default_cipher_driver` (the OpenSSL backend). This gives external code access to the built-in crypto without requiring a request context. If a user needs a custom driver, they can still use `csilk_ctx_set_cipher_driver()` on a request context and the `_csilk_*` path internally.

## Files Changed

| Action | Path | Description |
|--------|------|-------------|
| Create | `include/csilk/core/cipher.h` | Public cipher API declarations |
| Modify | `src/crypto/cipher_dispatch.c` | Add 6 public wrapper functions |

## Constant Definitions

All size constants come from `include/csilk/drivers/cipher.h` (already included transitively):
- `CSILK_AES256_KEY_SIZE` = 32
- `CSILK_GCM_IV_SIZE` = 12
- `CSILK_GCM_TAG_SIZE` = 16
- `CSILK_RSA_KEY_SIZE` = 256 (output buffer for ciphertext/signature)
- `CSILK_RSA_SIGNATURE_SIZE` = 256

## Caller Impact

- Zero breaking changes. All existing `_csilk_*` functions remain unchanged.
- No changes to `cmake/sources.cmake` — `cipher_dispatch.c` is already in `CSILK_CORE_SOURCES`.
- New header `include/csilk/core/cipher.h` is not pulled in transitively — callers must explicitly `#include <csilk/core/cipher.h>`.

## Tests

Add to `tests/security/test_cipher.c` (already tests the internal dispatch):
- Roundtrip test for `csilk_symmetric_encrypt` / `csilk_symmetric_decrypt`
- Tag mismatch rejection for decrypt
- Null parameter rejection
- RSA keypair generation roundtrip

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| `csilk_symmetric_encrypt` name conflicts with any future API | Low | Prefix follows existing pattern (`csilk_hmac_sha256`, `csilk_bcrypt_hash`) |
| Ciphertext buffer size confusion (ciphertext may include tag) | Low | Doc comment clarifies `ciphertext_len` is actual output length |
| Caller must free `private_key` / `public_key` from `csilk_rsa_generate_keypair` | None | Same pattern as `_csilk_generate_keypair` — caller allocates buffers, driver fills them |

## Out of Scope

- `csilk_jwt_sign` / `csilk_jwt_verify` as standalone public functions — JWT is already covered by `csilk_jwt_generate()` / `csilk_jwt_verify()` in `middleware.h`
- Context-aware public cipher functions (would require passing `csilk_ctx_t*`, defeating the purpose)
- HMAC-SHA256 cipher operation (already covered by `csilk_hmac_sha256()` in `hash.h`)
