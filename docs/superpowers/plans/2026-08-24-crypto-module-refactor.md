# Crypto Module Refactor Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `crypto.c` into two files by responsibility, and move URL utilities out of the crypto directory.

**Architecture:** `crypto.c` currently mixes two VTable dispatch layers (crypto primitives + cipher operations). We extract the cipher layer into `cipher_dispatch.c`, slim `crypto.c` to primitives only, and move `url.c` to `src/core/primitives/`. All public APIs and test files remain unchanged.

**Tech Stack:** C23, OpenSSL, CMake

---

## File Map

| Action | Path | Purpose |
|--------|------|---------|
| Create | `src/crypto/cipher_dispatch.c` | Cipher dispatch functions (symmetric, asymmetric, sign, JWT) |
| Modify | `src/crypto/crypto.c` | Remove cipher dispatch, keep primitives (~340 lines) |
| Move | `src/crypto/url.c` → `src/core/primitives/url.c` | URL decode/split — HTTP parsing, not crypto |
| Modify | `cmake/sources.cmake` | Update source list |

No files in `include/`, `tests/`, or public headers change.

---

## Chunk 1: Extract `cipher_dispatch.c`

### Task 1: Create `src/crypto/cipher_dispatch.c`

Extract lines 313–711 of `src/crypto/crypto.c` (from `extern csilk_cipher_driver_t` through end of file) into a new file.

**Step 1: Create the file**

```bash
sed -n '313,711p' src/crypto/crypto.c > src/crypto/cipher_dispatch.c
```

This produces a file containing:
- `extern csilk_cipher_driver_t csilk_default_cipher_driver;`
- `resolve_cipher()` (static)
- `_csilk_symmetric_encrypt/decrypt`
- `_csilk_generate_keypair`
- `_csilk_asymmetric_encrypt/decrypt`
- `_csilk_sign/verify`
- `_csilk_jwt_sign/verify`

**Step 2: Add header to the new file**

The extracted content needs a file header. Edit the top of `src/crypto/cipher_dispatch.c`:

```c
/**
 * @file cipher_dispatch.c
 * @brief Cipher operation dispatch layer.
 *
 * Implements context-aware dispatch for symmetric encryption,
 * asymmetric encryption, signing, and JWT operations via the
 * csilk_cipher_driver_t VTable.  Falls back to the built-in
 * OpenSSL implementation when no driver is set.
 *
 * @copyright MIT License
 */

#include "core/ctx/ctx_internal.h"
#include <stdlib.h>
#include "csilk/core/internal.h"
#include "csilk/core/crypto.h"
#include "csilk/drivers/cipher.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
```

**Step 3: Verify the file compiles by checking its content**

```bash
wc -l src/crypto/cipher_dispatch.c
head -30 src/crypto/cipher_dispatch.c
tail -10 src/crypto/cipher_dispatch.c
```

Expected: ~399 lines, starts with the header comment, ends with `_csilk_jwt_verify`.

### Task 2: Slim `crypto.c`

**Step 4: Truncate `crypto.c` to lines 1–312**

```bash
sed -i '313,711d' src/crypto/crypto.c
```

**Step 5: Update the header comment**

Edit the file comment at the top of `src/crypto/crypto.c` (lines 1–15):

```c
/**
 * @file crypto.c
 * @brief Cryptographic primitives: SHA-256, HMAC, UUID, random, nonce.
 *
 * Implements:
 *   - SHA-256 : HMAC, session integrity
 *   - HMAC-SHA256 : Keyed-hash message authentication
 *   - UUID generation (RFC 4122)
 *   - Platform RNG (getrandom/arc4random/CryptGenRandom/urandom)
 *   - Monotonic nonce fallback for GCM safety
 *
 * Cipher dispatch (AES/RSA/JWT) moved to cipher_dispatch.c.
 * @copyright MIT License
 */
```

**Step 6: Verify the truncated file**

```bash
wc -l src/crypto/crypto.c
tail -20 src/crypto/crypto.c
```

Expected: ~312 lines, ends with the nonce function closing brace.

### Task 3: Update CMake

**Step 7: Edit `cmake/sources.cmake`**

Current state (line 28):
```cmake
    src/crypto/url.c
    src/crypto/uuid.c
    src/crypto/crypto.c
```

Replace with:
```cmake
    src/crypto/cipher_dispatch.c
    src/crypto/uuid.c
    src/crypto/crypto.c
```

And add `src/core/primitives/url.c` in the primitives section (after `src/core/primitives/recovery.c` or alphabetically near `query.c`).

**Step 8: Build to verify**

```bash
cmake --build build --target csilk_core -j$(nproc) 2>&1 | head -50
```

Expected: Clean build with no errors or warnings.

**Step 9: Run affected tests**

```bash
ctest --test-dir build -R "test_crypto|test_bcrypt|test_uuid|test_cipher|test_crypto_primitives|test_url_decode|test_url_ext|test_query" --timeout 10 --output-on-failure
```

Expected: All tests pass.

### Task 4: Commit

```bash
git add src/crypto/cipher_dispatch.c src/crypto/crypto.c src/core/primitives/url.c cmake/sources.cmake
git rm src/crypto/url.c
git commit -m "refactor ♻️: split crypto module by responsibility"
```

---

## Chunk 2: Full Test Suite & Verification

### Task 5: Run full test suite

```bash
ctest --test-dir build -E test_integration --timeout 30 --output-on-failure
```

Expected: All tests pass.

### Task 6: Format check

```bash
cmake --build build --target check-format 2>&1 | tail -20
```

If any formatting issues, run:
```bash
cmake --build build --target format
git add -u
git commit -m "style 🎨: apply clang-format after crypto refactor"
```

### Task 7: Verify no dangling references

```bash
grep -rn "src/crypto/url" cmake/ && echo "FOUND - needs fix" || echo "OK"
grep -rn "cipher_dispatch" cmake/ && echo "FOUND" || echo "MISSING"
```

---

## Verification Checklist

- [ ] `src/crypto/cipher_dispatch.c` exists with correct content (~399 lines)
- [ ] `src/crypto/crypto.c` is slimmed to ~312 lines, header updated
- [ ] `src/core/primitives/url.c` exists (moved from `src/crypto/url.c`)
- [ ] `src/crypto/url.c` removed
- [ ] `cmake/sources.cmake` references both new paths, not the old one
- [ ] `csilk_core` target builds without errors
- [ ] All crypto/bcrypt/uuid/cipher/url tests pass
- [ ] Full test suite passes (excluding integration)
- [ ] Clang-format check passes
- [ ] Git commit created
