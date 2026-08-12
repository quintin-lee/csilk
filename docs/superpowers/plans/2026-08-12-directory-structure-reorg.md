# Directory Structure Reorganization Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize src/ and tests/ directories so files are in logically correct locations, while preserving all public APIs and build behavior.

**Architecture:** Phase-gated file moves using `git mv`, with CMake updates applied per phase so the build passes after each phase. No function signatures, no header paths for public APIs, no test behavior changes.

**Tech Stack:** C23, CMake 3.11+, GNU make, CTest

---

## Phase 0: Pre-flight — verify clean build baseline

- [ ] **Step 1: Confirm current build passes**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | tail -5
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -20
```

Expected: 168/168 tests pass, zero compiler warnings. Record this baseline.

- [ ] **Step 2: Ensure working tree is clean**

```bash
git status --short
```

Expected: nothing dirty. If dirty, commit or stash first.

---

## Phase 1: Create `src/crypto/` — move utility files from `src/core/server/`

**Files moved:**

| From | To | Include changes |
|------|-----|----------------|
| `src/core/server/base64.c` | `src/crypto/base64.c` | None |
| `src/core/server/sha1.c` | `src/crypto/sha1.c` | None |
| `src/core/server/url.c` | `src/crypto/url.c` | Change `#include "../ctx/ctx_internal.h"` → `#include "core/ctx/ctx_internal.h"` |
| `src/core/server/uuid.c` | `src/crypto/uuid.c` | None |
| `src/core/server/utils.c` | `src/crypto/utils.c` | Change `#include "../ctx/ctx_internal.h"` → `#include "core/ctx/ctx_internal.h"` |

**Why these include changes:** `base64.c`, `sha1.c`, `uuid.c` only use `#include "csilk/..."` absolute paths which continue to resolve via the existing `include_directories(include)` in `cmake/dependencies.cmake`. `url.c` and `utils.c` use relative `../ctx/ctx_internal.h` — this path breaks when the file moves two levels deeper, so they must switch to the `src/`-relative form `core/ctx/ctx_internal.h`.

- [ ] **Step 1: Create directory and move files**

```bash
mkdir -p src/crypto
git mv src/core/server/base64.c src/crypto/base64.c
git mv src/core/server/sha1.c src/crypto/sha1.c
git mv src/core/server/url.c src/crypto/url.c
git mv src/core/server/uuid.c src/crypto/uuid.c
git mv src/core/server/utils.c src/crypto/utils.c
```

- [ ] **Step 2: Fix relative includes in url.c and utils.c**

In `src/crypto/url.c`, replace:
```c
#include "../ctx/ctx_internal.h"
```
with:
```c
#include "core/ctx/ctx_internal.h"
```

In `src/crypto/utils.c`, replace:
```c
#include "../ctx/ctx_internal.h"
```
with:
```c
#include "core/ctx/ctx_internal.h"
```

- [ ] **Step 3: Update cmake/sources.cmake**

Add new set and update CSILK_CORE_SOURCES:

```cmake
set(CSILK_CRYPTO_SOURCES
    src/crypto/base64.c
    src/crypto/sha1.c
    src/crypto/url.c
    src/crypto/uuid.c
    src/crypto/utils.c
)
```

Remove the 5 moved files from `CSILK_CORE_SOURCES` list (lines ~37-41 in the current file).

Add `${CSILK_CRYPTO_SOURCES}` to the `CSILK_SOURCES` aggregation block at the bottom.

- [ ] **Step 4: Build and test**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -5
```

Expected: clean build, all tests pass. If any include errors, check the relative path fix above.

- [ ] **Step 5: Commit**

```bash
cd /data/home/quintin/workspace/source/c/csilk
git add src/crypto/ src/core/server/base64.c src/core/server/sha1.c src/core/server/url.c src/core/server/uuid.c src/core/server/utils.c cmake/sources.cmake
git commit -m "refactor(core): 🎸 move crypto utilities from src/core/server/ to new src/crypto/ module"
```

---

## Phase 2: Create `src/security/` — move bcrypt and blowfish_sboxes

**Files moved:**

| From | To | Include changes |
|------|-----|----------------|
| `src/core/bcrypt.c` | `src/security/bcrypt.c` | Change `#include "csilk/core/blowfish_sboxes.h"` → `#include "blowfish_sboxes.h"` |
| `include/csilk/core/blowfish_sboxes.h` | `src/security/blowfish_sboxes.h` | None (0 external consumers) |

**Note:** `blowfish_sboxes.h` is only included by `bcrypt.c`. It contains raw S-box data constants (64 lines of hex literals) — not a public API header. Moving it to `src/security/` as an internal header is correct.

- [ ] **Step 1: Create directory and move files**

```bash
mkdir -p src/security
git mv src/core/bcrypt.c src/security/bcrypt.c
git mv include/csilk/core/blowfish_sboxes.h src/security/blowfish_sboxes.h
```

- [ ] **Step 2: Fix include in bcrypt.c**

In `src/security/bcrypt.c`, replace:
```c
#include "csilk/core/blowfish_sboxes.h"
```
with:
```c
#include "blowfish_sboxes.h"
```

(Both `include/` and `src/` are in the compiler include path, so the bare filename resolves to `src/security/blowfish_sboxes.h`.)

- [ ] **Step 3: Update cmake/sources.cmake**

Remove `src/core/bcrypt.c` from `CSILK_CORE_SOURCES`.

Add to `CSILK_SECURITY_SOURCES`:
```cmake
set(CSILK_SECURITY_SOURCES
    src/security/bcrypt.c
    src/drivers/perm/perm.c
)
```

(Note: `CSILK_SECURITY_SOURCES` currently exists but only contains `src/drivers/perm/perm.c` — append bcrypt there instead of creating a separate set, since the module naming in CMake uses "security" for both perm and bcrypt.)

Actually, reviewing the current sources.cmake: `CSILK_SECURITY_SOURCES` only has `src/drivers/perm/perm.c`. The new bcrypt belongs with it semantically. Update to:
```cmake
set(CSILK_SECURITY_SOURCES
    src/drivers/perm/perm.c
    src/security/bcrypt.c
)
```

- [ ] **Step 4: Build and test**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -5
```

Expected: clean build, all tests pass (including `test_bcrypt`).

- [ ] **Step 5: Commit**

```bash
git add src/security/ include/csilk/core/blowfish_sboxes.h cmake/sources.cmake
git commit -m "refactor(security): 🎸 move bcrypt and blowfish_sboxes to new src/security/ module"
```

---

## Phase 3: Move `admin.c` from `src/core/config/` to `src/app/`

**Files moved:**

| From | To | Include changes |
|------|-----|----------------|
| `src/core/config/admin.c` | `src/app/admin.c` | None needed |

**Why no include changes:** The CMake build already sets `include_directories(include)` and `include_directories(src)` in `cmake/dependencies.cmake`. All `#include "csilk/..."` paths in `admin.c` continue to resolve correctly regardless of its location under `src/`. The existing `#include "util/flamegraph.h"` is a relative path that also continues to work since `src/` is in the include path.

- [ ] **Step 1: Move the file**

```bash
git mv src/core/config/admin.c src/app/admin.c
```

- [ ] **Step 2: Update cmake/sources.cmake**

Remove `src/core/config/admin.c` from `CSILK_CORE_SOURCES`.

Add to `CSILK_APP_SOURCES`:
```cmake
set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/app_routes.c
    src/app/group.c
    src/app/admin.c
)
```

- [ ] **Step 3: Build and test**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -5
```

Expected: clean build, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/app/admin.c src/core/config/admin.c cmake/sources.cmake
git commit -m "refactor(app): 🎸 move admin.c from src/core/config/ to src/app/ where it logically belongs"
```

---

## Phase 4: Reorganize test directories

**Files moved:**

| From | To | Test name (CMake) |
|------|-----|---|
| `tests/data/test_cipher.c` | `tests/security/test_cipher.c` | `test_cipher` |
| `tests/data/test_crypto_driver.c` | `tests/security/test_crypto_driver.c` | `test_crypto_driver` |
| `tests/data/test_db.c` | `tests/drivers/db/test_db.c` | `test_db` |
| `tests/data/test_mongodb.c` | `tests/drivers/db/test_mongodb.c` | `test_mongodb` |

**Note on test_utils:** `test_cipher.c` and `test_crypto_driver.c` call `csilk_test_ctx_new()` / `csilk_test_ctx_free()` which are defined in `src/core/test_utils.c` and compiled into the `csilk` library. Moving these test files does NOT require moving test_utils — the library linkage handles it transparently.

**Note on config_load_test.yaml:** This file in `tests/fixtures/` is an orphaned fixture (no test references it). Left untouched.

- [ ] **Step 1: Move test files**

```bash
git mv tests/data/test_cipher.c tests/security/test_cipher.c
git mv tests/data/test_crypto_driver.c tests/security/test_crypto_driver.c
mkdir -p tests/drivers/db
git mv tests/data/test_db.c tests/drivers/db/test_db.c
git mv tests/data/test_mongodb.c tests/drivers/db/test_mongodb.c
```

- [ ] **Step 2: Update cmake/tests.cmake**

Current state (lines ~225-235):
```cmake
set(CSILK_DATA_TESTS
    test_cipher
    test_crypto_driver
    test_db
    test_mongodb
    test_db_sqlite
    test_db_registry
)
set(CSILK_DATA_TEST_DIRS
    data;data;data;data;drivers;drivers
)
```

Replace with:
```cmake
set(CSILK_DATA_TESTS
    test_db_sqlite
    test_db_registry
)
set(CSILK_DATA_TEST_DIRS
    drivers;drivers
)
```

Move `test_cipher` and `test_crypto_driver` into the security test lists. Find the existing security sections and update:

```cmake
set(CSILK_SECURITY_TESTS
    test_perm
    test_perm_ext
    test_crypto_primitives
    test_jwt_security
    test_uuid
    test_bcrypt
    test_cipher
    test_crypto_driver
)
set(CSILK_SECURITY_TEST_DIRS
    security;security;security;security;security;security
    security;security
)
```

Add `test_db` and `test_mongodb` to the AI/tests dirs (or create a dedicated DB test set). The simplest approach: add them to the existing AI test dirs since they live under `tests/drivers/`:

```cmake
set(CSILK_AI_TESTS
    test_ai
    test_ai_ext
    test_vector_db
    test_vector_simd
    test_vector_hnsw
    test_vector_db_embedded
    test_db
    test_mongodb
)
set(CSILK_AI_TEST_DIRS
    drivers;drivers;drivers/vector;drivers/vector;drivers/vector
    drivers/db;drivers/db
)
```

- [ ] **Step 3: Build and test**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -10
```

Expected: all tests pass. Verify `test_cipher`, `test_crypto_driver`, `test_db`, `test_mongodb` appear in the test list.

- [ ] **Step 4: Commit**

```bash
git add tests/security/test_cipher.c tests/security/test_crypto_driver.c tests/drivers/db/ cmake/tests.cmake
git commit -m "refactor(tests): 🎸 reorganize tests/data/ into tests/security/ and tests/drivers/db/"
```

---

## Phase 5: CMake cleanup — remove CSILK_DATA_SOURCES

- [ ] **Step 1: Update cmake/sources.cmake**

Current state:
```cmake
set(CSILK_DATA_SOURCES
    src/drivers/db/db.c
)
```
and later:
```cmake
set(CSILK_SOURCES
    ...
    ${CSILK_DATA_SOURCES}
    ${CSILK_DRIVER_SOURCES}
    ...
)
```

Replace `CSILK_DATA_SOURCES` with inline inclusion in `CSILK_DRIVER_SOURCES`:
```cmake
set(CSILK_DRIVER_SOURCES
    src/drivers/ai/ollama.c
    src/drivers/ai/openai.c
    src/drivers/cipher/openssl.c
    src/drivers/perm/simple.c
    src/drivers/db/db.c
    src/drivers/vector/vector.c
    ...
)
```

Remove the `CSILK_DATA_SOURCES` set entirely. Remove `${CSILK_DATA_SOURCES}` from the `CSILK_SOURCES` aggregation block.

- [ ] **Step 2: Build and test**

```bash
cd /data/home/quintin/workspace/source/c/csilk/build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure 2>&1 | tail -5
```

Expected: clean build, all tests pass.

- [ ] **Step 3: Final verification**

```bash
# Check no stray files remain
find src/core/server -name "*.c" -o -name "*.h" | sort
find src/core -maxdepth 1 -name "*.c" | sort
find tests/data -name "*.c" 2>/dev/null | sort
git status --short
```

Expected:
- `src/core/server/` has only: connection.c, server_lifecycle.c, server_shutdown.c, server_worker.c
- `src/core/` has no stray .c files
- `tests/data/` has no .c files (only possibly non-test artifacts)
- `git status` shows only committed changes

- [ ] **Step 4: Commit**

```bash
git add cmake/sources.cmake
git commit -m "refactor(build): 🎸 remove CSILK_DATA_SOURCES, merge db.c into CSILK_DRIVER_SOURCES"
```

---

## Final Validation

- [ ] **Step 1: Full clean build from scratch**

```bash
cd /data/home/quintin/workspace/source/c/csilk
rm -rf build && mkdir build && cd build
cmake .. 2>&1 | grep -iE 'error|warning' | head -10
make -j$(nproc) 2>&1 | grep -cE 'warning|error' || true
ctest --output-on-failure 2>&1 | tail -5
```

Expected: zero warnings, zero errors, 168/168 tests pass.

- [ ] **Step 2: Confirm git history preserved**

```bash
git log --oneline -10
git log --name-status --reverse | head -40
```

Expected: all `git mv` operations appear as renames (not deletes + adds).
