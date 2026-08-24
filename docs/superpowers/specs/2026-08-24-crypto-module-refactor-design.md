# Crypto Module Refactor Design

**Date:** 2026-08-24
**Status:** Approved
**Author:** Agnes (Claude)

## Problem

`src/crypto/crypto.c` (711 lines) mixes two independent dispatch layers:

| Layer | VTable | Functions |
|-------|--------|-----------|
| Crypto primitives | `csilk_crypto_driver_t` | SHA-256, HMAC, UUID, fill_random, SHA-1, bcrypt |
| Cipher operations | `csilk_cipher_driver_t` | symmetric encrypt/decrypt, asymmetric encrypt/decrypt, sign/verify, JWT sign/verify, generate_keypair |

Both layers share the same dispatch pattern (`resolve_cipher()`) but serve different concerns. Additionally, `src/crypto/url.c` contains HTTP parsing utilities with no cryptographic relationship.

## Current State

### Source Layout

```
src/crypto/
├── crypto.c      # 711 lines: SHA-256, HMAC, UUID, RNG, nonce, cipher dispatch
├── bcrypt.c      # 455 lines: Eksblowfish bcrypt (self-contained, stable)
├── sha1.c        # ~30 lines: OpenSSL SHA-1 wrapper (WebSocket only)
├── base64.c      # ~150 lines: RFC 4648 encoding
├── uuid.c        # ~60 lines: RFC 4122 UUID v4
├── url.c         # ~90 lines: URL percent-decoding, path/query splitting
└── blowfish_sboxes.h  # ~500 lines: Static S-box constants
```

### Public API Headers

| Header | Exports |
|--------|---------|
| `include/csilk/core/crypto.h` | `csilk_crypto_driver_t`, `csilk_jwt_*` types, `csilk_crypto_fill_random()`, `csilk_crypto_generate_nonce()` |
| `include/csilk/core/crypto_dispatch.h` | All `_csilk_*` internal dispatchers |
| `include/csilk/core/hash.h` | `csilk_sha256_*`, `csilk_sha1_*`, `csilk_hmac_sha256()` |
| `include/csilk/core/bcrypt.h` | `csilk_bcrypt_hash()`, `csilk_bcrypt_verify()` |
| `include/csilk/core/codec.h` | `csilk_base64_*`, `csilk_url_decode()` |
| `include/csilk/core/context.h` | `csilk_split_url()` |

### Callers of `csilk_url_decode` / `csilk_split_url`

- `src/core/http/http1_parse.c` — URL decode during request parsing
- `src/core/http/h2_callbacks.c` — URL split for HTTP/2 PUSH_PROMISE
- `src/core/http/h2_response.c` — URL split for pushed responses
- `src/core/primitives/query.c` — URL decode query parameters

### CMake Integration

`src/crypto/url.c` and `src/crypto/crypto.c` are both in `CSILK_CORE_SOURCES`.

## Proposed Changes

### Change 1: Extract `cipher_dispatch.c` from `crypto.c`

Create `src/crypto/cipher_dispatch.c` containing all cipher-layer dispatch functions:

```c
// src/crypto/cipher_dispatch.c
// Contents:
//   static csilk_cipher_driver_t* resolve_cipher(csilk_ctx_t* c)
//   _csilk_symmetric_encrypt()
//   _csilk_symmetric_decrypt()
//   _csilk_generate_keypair()
//   _csilk_asymmetric_encrypt()
//   _csilk_asymmetric_decrypt()
//   _csilk_sign()
//   _csilk_verify()
//   _csilk_jwt_sign()
//   _csilk_jwt_verify()
```

`resolve_cipher()` remains `static` in both files (compilation unit isolation). Both files link into `libcsilk-core.a` — no symbol collision.

### Change 2: Slim `crypto.c` to Primitive Layer Only

After extraction, `crypto.c` contains only:

```c
// src/crypto/crypto.c (remaining functions)
csilk_sha256_init/update/final()     // OpenSSL SHA-256 context ops
csilk_hmac_sha256()                  // OpenSSL HMAC-SHA256
_csilk_hmac_sha256()                 // Context-aware dispatch
_csilk_sha1()                        // Context-aware dispatch
_csilk_generate_uuid()               // Context-aware dispatch
_csilk_fill_random()                 // Platform RNG + context dispatch
csilk_crypto_fill_random()           // Public convenience wrapper
_csilk_bcrypt_hash()                 // Context-aware dispatch
csilk_crypto_generate_nonce()        // Nonce with monotonic fallback
```

New header comment:
```c
/**
 * @file crypto.c
 * @brief Cryptographic primitives: SHA-256, HMAC, UUID, random, nonce.
 *
 * Implements:
 *   - SHA-256 : HMAC, JWT signing, session integrity
 *   - HMAC-SHA256 : Keyed-hash message authentication
 *   - UUID generation (RFC 4122)
 *   - Platform RNG (getrandom/arc4random/CryptGenRandom/urandom)
 *   - Monotonic nonce fallback for GCM safety
 *
 * Cipher dispatch (AES/RSA/JWT) moved to cipher_dispatch.c.
 */
```

### Change 3: Move `url.c` to `src/core/primitives/`

```
src/crypto/url.c  →  src/core/primitives/url.c
```

No code changes needed — `url.c` includes:
- `"core/ctx/ctx_internal.h"` — already resolvable from new location
- `"csilk/core/internal.h"` — already resolvable from new location

Public declarations stay in place:
- `csilk_url_decode()` declared in `include/csilk/core/codec.h`
- `csilk_split_url()` declared in `include/csilk/core/context.h`

Callers use include paths relative to source root; no caller modifications required.

### Change 4: Update `cmake/sources.cmake`

```cmake
# CSILK_CORE_SOURCES:
# - Remove: src/crypto/url.c
# - Add:    src/crypto/cipher_dispatch.c
# - Add:    src/core/primitives/url.c  (if not already present)
```

## Files Not Changing

| File | Reason |
|------|--------|
| `bcrypt.c` | Self-contained, tested, stable |
| `blowfish_sboxes.h` | Part of bcrypt implementation |
| `sha1.c` | Correctly separated already |
| `base64.c` | No issues |
| `uuid.c` | No issues |
| All public headers | API signatures unchanged |
| All test files | No behavioral changes |

## Final Directory Layout

```
src/crypto/
├── cipher_dispatch.c   ← NEW (from crypto.c lines 327-711)
├── crypto.c            ← SLIMMED (~340 lines, primitives only)
├── bcrypt.c            ← unchanged
├── sha1.c              ← unchanged
├── base64.c            ← unchanged
├── uuid.c              ← unchanged
└── blowfish_sboxes.h   ← unchanged

src/core/primitives/
├── url.c               ← MOVED from src/crypto/url.c
├── ...
```

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| `resolve_cipher` duplicate symbol | None | Both definitions are `static` — link-time isolation |
| URL include path breakage | None | Include paths relative to source root unchanged |
| Test failures | None | No behavioral changes, only file movements |
| Missing build target update | Low | Update `cmake/sources.cmake` explicitly |
| Header dependency cycles | None | No new includes introduced |

## Verification Plan

```bash
# Build
cmake --build build --target csilk_core -j$(nproc)

# Run affected tests
ctest --test-dir build \
  -R "test_crypto|test_bcrypt|test_uuid|test_cipher|test_crypto_primitives" \
  --timeout 10 --output-on-failure

# Run full test suite
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

## Out of Scope

- Replacing bcrypt with libsodium/Argon2id (future work)
- Removing SHA-1 dependency (WebSocket handshake requires it per RFC 6455)
- Merging `csilk_crypto_driver_t` and `csilk_cipher_driver_t` (different VTable semantics)
- Moving `base64.c` or `uuid.c` (correctly located already)
