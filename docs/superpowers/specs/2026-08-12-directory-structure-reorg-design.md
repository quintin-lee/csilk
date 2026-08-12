# Spec: Directory Structure Reorganization

**Date**: 2026-08-12
**Scope**: Reorganize src/ and include/ directory tree to match logical module boundaries
**Constraint**: Zero API changes — all function signatures, header names, and CMake targets remain identical
**Method**: Use `git mv` for every move to preserve history

---

## Current Problems

### 1. `src/core/server/` is a junk drawer
9 files mixed with unrelated responsibilities:

| File | Actual Module | Current Location |
|------|--------------|-----------------|
| `base64.c` | crypto / codec | `src/core/server/` |
| `sha1.c` | crypto / hash | `src/core/server/` |
| `url.c` | crypto / url | `src/core/server/` |
| `uuid.c` | crypto / uuid | `src/core/server/` |
| `utils.c` | crypto / crypto | `src/core/server/` |
| `connection.c` | server | `src/core/server/` ✅ |
| `server_lifecycle.c` | server | `src/core/server/` ✅ |
| `server_shutdown.c` | server | `src/core/server/` ✅ |
| `server_worker.c` | server | `src/core/server/` ✅ |

### 2. `src/core/` top-level has two stray files
- `bcrypt.c` — security module, belongs in `src/security/`
- `test_utils.c` — test infrastructure, should not be in production source tree

### 3. `include/csilk/core/blowfish_sboxes.h` leaks internal detail
Only `src/core/bcrypt.c` includes it. It should be an internal header, not public API.

### 4. `src/core/config/admin.c` is misplaced
Implementation is in `src/core/config/` but its includes are `csilk/app/admin.h` + `csilk/app/app.h`. It belongs in `src/app/`.

### 5. `tests/data/` contains misnamed tests
- `test_cipher.c` → security test
- `test_crypto_driver.c` → security test
- `test_db.c` → database driver test
- `test_mongodb.c` → database driver test

These belong in `tests/security/` and `tests/drivers/db/`.

### 6. CMake variable naming inconsistency
`CSILK_DATA_SOURCES` points only to `src/drivers/db/db.c` — semantically it should be `CSILK_DB_SOURCES` or merged into `CSILK_DRIVER_SOURCES`.

---

## Proposed Target Structure

```
src/
├── core/
│   ├── config/          ← admin.c removed, rest stays
│   ├── ctx/             ← unchanged
│   ├── http/            ← unchanged
│   ├── io/              ← unchanged
│   ├── json/            ← unchanged
│   ├── plugin/          ← unchanged
│   ├── primitives/      ← unchanged
│   ├── server/          ←只剩 4 个 server 相关文件
│   ├── uring/           ← unchanged
│   └── cache/           ← unchanged
├── crypto/              ← NEW: 从 server/ 移入 base64/sha1/url/uuid/utils
├── security/            ← NEW: bcrypt + blowfish_sboxes
├── app/                 ← admin.c 从 config/ 移入
├── drivers/             ← unchanged
├── messaging/           ← unchanged
├── middleware/          ← unchanged
├── protocols/           ← unchanged
├── reflection/          ← unchanged
├── util/                ← unchanged
└── workflow/            ← unchanged

include/csilk/
├── core/
│   ├── crypto/          ← NEW: codec.h, hash.h, crypto.h, crypto_dispatch.h
│   └── ... (其余按模块归入子目录，顶层只保留 types.h, errors.h, internal.h, server.h)
└── ... (其余不变)

tests/
├── security/            ← 合并 tests/data/ 中的 test_cipher.c + test_crypto_driver.c
├── drivers/
│   └── db/              ← 新建：移入 test_db.c + test_mongodb.c
└── ... (其余不变)
```

---

## Change List (Atomic Operations)

### Phase 1: Create `src/crypto/` — move utility files from `src/core/server/`

| From | To | Reason |
|------|-----|--------|
| `src/core/server/base64.c` | `src/crypto/base64.c` | Codec utility, not server logic |
| `src/core/server/sha1.c` | `src/crypto/sha1.c` | Hash utility, not server logic |
| `src/core/server/url.c` | `src/crypto/url.c` | URL utility, not server logic |
| `src/core/server/uuid.c` | `src/crypto/uuid.c` | UUID utility, not server logic |
| `src/core/server/utils.c` | `src/crypto/utils.c` | Crypto/hash utility, not server logic |

Include path changes needed:
- `base64.c`: `#include "csilk/core/codec.h"` → no change needed (codec.h stays in include/csilk/core/)
- `sha1.c`: `#include "csilk/core/hash.h"` → no change needed
- `url.c`: `#include "csilk/core/internal.h"` and `#include "../ctx/ctx_internal.h"` → `#include "core/ctx/ctx_internal.h"`
- `uuid.c`: `#include "csilk/core/crypto_dispatch.h"` etc → no change needed
- `utils.c`: `#include "csilk/core/crypto.h"` etc → no change needed; `#include "../ctx/ctx_internal.h"` → `#include "core/ctx/ctx_internal.h"`

### Phase 2: Create `src/security/` — move bcrypt and blowfish

| From | To | Reason |
|------|-----|--------|
| `src/core/bcrypt.c` | `src/security/bcrypt.c` | Security module |
| `include/csilk/core/blowfish_sboxes.h` | `src/security/blowfish_sboxes.h` | Internal detail, not public API |

Include path changes:
- `bcrypt.c`: `#include "csilk/core/blowfish_sboxes.h"` → `#include "blowfish_sboxes.h"` (same-dir include after move)
- `bcrypt.c`: `#include "csilk/core/bcrypt.h"` → no change needed (public header stays at include/csilk/core/bcrypt.h)

### Phase 3: Move `admin.c` to `src/app/`

| From | To | Reason |
|------|-----|--------|
| `src/core/config/admin.c` | `src/app/admin.c` | Includes `csilk/app/admin.h` and `csilk/app/app.h` |

**No include path changes needed.** The CMake build already sets `include_directories(include)` and `include_directories(src)` (in `cmake/dependencies.cmake`), so all existing `#include "csilk/..."` paths in `admin.c` continue to resolve correctly after the move. Only `#include "util/flamegraph.h"` may optionally be updated to `#include "../util/flamegraph.h"`, but the existing relative path also works.

### Phase 4: Reorganize test directories

| From | To |
|------|-----|
| `tests/data/test_cipher.c` | `tests/security/test_cipher.c` |
| `tests/data/test_crypto_driver.c` | `tests/security/test_crypto_driver.c` |
| `tests/data/test_db.c` | `tests/drivers/db/test_db.c` |
| `tests/data/test_mongodb.c` | `tests/drivers/db/test_mongodb.c` |

Note: `tests/fixtures/config_load_test.yaml` is an orphaned file (no test references it) and is left untouched.

### Phase 5: Update CMake

1. **cmake/sources.cmake**:
   - Add `CSILK_CRYPTO_SOURCES` set with base64/sha1/url/uuid/utils
   - Add `CSILK_SECURITY_SOURCES` set with bcrypt.c
   - Remove `src/core/server/base64.c`, `sha1.c`, `url.c`, `uuid.c`, `utils.c` from `CSILK_CORE_SOURCES`
   - Remove `src/core/bcrypt.c` from `CSILK_CORE_SOURCES`
   - Move `src/core/config/admin.c` to `CSILK_APP_SOURCES`
    - Remove `CSILK_DATA_SOURCES` and inline its single file (`src/drivers/db/db.c`) into `CSILK_DRIVER_SOURCES`

2. **cmake/tests.cmake**:
   - Move `test_cipher` and `test_crypto_driver` from `CSILK_DATA_TESTS`/`CSILK_DATA_TEST_DIRS` to `CSILK_SECURITY_TESTS`/`CSILK_SECURITY_TEST_DIRS`
   - Move `test_db` and `test_mongodb` from `CSILK_DATA_TESTS`/`CSILK_DATA_TEST_DIRS` to `CSILK_AI_TESTS`/`CSILK_AI_TEST_DIRS` (or create a new `CSILK_DB_TESTS` set under `drivers/db/`)
   - Remove empty `CSILK_DATA_TESTS`/`CSILK_DATA_TEST_DIRS` sets

---

## Files NOT Moved (and why)

| File | Stays | Reason |
|------|-------|--------|
| `src/core/server/connection.c` | `src/core/server/` | Core server I/O logic |
| `src/core/server/server_lifecycle.c` | `src/core/server/` | Server lifecycle |
| `src/core/server/server_shutdown.c` | `src/core/server/` | Server shutdown |
| `src/core/server/server_worker.c` | `src/core/server/` | Server worker threads |
| `include/csilk/core/*.h` (most) | `include/csilk/core/` | Public API surface — header location is part of API contract |
| `include/csilk/core/admin.h` | stays (re-export wrapper) | Already just re-exports `csilk/app/admin.h` |
| `include/csilk/core/bcrypt.h` | stays | Public API — consumers include this path |

---

## Verification Plan

After all moves:
1. `cmake .. && make -j$(nproc)` — zero warnings, zero errors
2. `ctest --output-on-failure` — all 168 tests pass
3. `git status` — only renames, no content changes except #include paths
4. `git log --name-status` — confirms all moves tracked via `git mv`

---

## Out of Scope

- Creating subdirectories under `include/csilk/core/` (header reorganization is a separate concern — headers are part of the public API contract and moving them would be a breaking change for consumers)
- Merging `src/drivers/perm/` and `src/drivers/cipher/` into `src/security/` (perm and cipher are pluggable driver interfaces, not pure crypto)
- Removing `CSILK_DATA_SOURCES` variable rename (low impact, can be done separately)
