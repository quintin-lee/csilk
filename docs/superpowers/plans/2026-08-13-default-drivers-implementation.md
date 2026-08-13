# Default Driver Binding & Context Driver Accessors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind default crypto/cipher drivers to `csilk_server_t` and `csilk_ctx_t` on initialization so driver pointers are non-NULL by default, simplify internal dispatch logic, and add public driver getter accessors (`csilk_ctx_get_*_driver`).

**Architecture:** A global `csilk_default_crypto_driver` singleton will be exported from `src/crypto/crypto.c`. `_csilk_ctx_init` and `csilk_server_init` will populate `crypto_driver` and `cipher_driver` with defaults if unspecified. Public getter APIs on `csilk_ctx_t` will allow upper layers and custom middleware to access the bound driver vtables.

**Tech Stack:** C23, CMake, Clang, CTest.

---

### Task 1: Define `csilk_default_crypto_driver` Singleton and Update Crypto Dispatch Helpers

**Files:**
- Modify: `include/csilk/core/crypto.h`
- Modify: `src/crypto/crypto.c`
- Test: `tests/security/test_crypto_driver.c`

- [ ] **Step 1: Write failing test in `tests/security/test_crypto_driver.c`**

Add a test function `test_default_crypto_driver_singleton()` verifying `csilk_default_crypto_driver` is non-NULL and its callbacks execute correctly:

```c
static void test_default_crypto_driver_singleton(void) {
    printf("Testing csilk_default_crypto_driver singleton...\n");
    assert(csilk_default_crypto_driver.sha256 != NULL);
    assert(csilk_default_crypto_driver.hmac_sha256 != NULL);
    assert(csilk_default_crypto_driver.generate_uuid != NULL);
    assert(csilk_default_crypto_driver.fill_random != NULL);
    assert(csilk_default_crypto_driver.sha1 != NULL);
    assert(csilk_default_crypto_driver.bcrypt_hash != NULL);

    uint8_t hash[32];
    csilk_default_crypto_driver.sha256((const uint8_t*)"test", 4, hash);
    assert(hash[0] == 0x9f); // SHA256 of "test" starts with 0x9f2b...

    printf("csilk_default_crypto_driver singleton passed!\n");
}
```
Add call to `test_default_crypto_driver_singleton()` in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_crypto_driver --output-on-failure`  
Expected: Compilation error (`csilk_default_crypto_driver` undeclared).

- [ ] **Step 3: Implement `csilk_default_crypto_driver` and update dispatchers**

In `include/csilk/core/crypto.h`, declare:
```c
extern csilk_crypto_driver_t csilk_default_crypto_driver;
```

In `src/crypto/crypto.c`, define wrapper functions and default driver:
```c
static void default_sha256_wrapper(const uint8_t* data, size_t len, uint8_t out[32]) {
    csilk_sha256(data, len, out);
}

static void default_hmac_sha256_wrapper(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]) {
    csilk_hmac_sha256(key, key_len, data, data_len, out);
}

static void default_generate_uuid_wrapper(char buf[CSILK_UUID_BUF_SIZE]) {
    csilk_generate_uuid(buf);
}

static int default_fill_random_wrapper(void* out, size_t len) {
    return csilk_crypto_fill_random(out, len);
}

static void default_sha1_wrapper(const uint8_t* data, size_t len, uint8_t out[20]) {
    csilk_sha1_ctx ctx;
    csilk_sha1_init(&ctx);
    csilk_sha1_update(&ctx, data, len);
    csilk_sha1_final(&ctx, out);
}

static void default_bcrypt_hash_wrapper(const char* password, size_t len, int cost, char hash[CSILK_BCRYPT_HASH_LEN]) {
    csilk_bcrypt_hash(password, len, cost, hash);
}

csilk_crypto_driver_t csilk_default_crypto_driver = {
    .sha256        = default_sha256_wrapper,
    .hmac_sha256   = default_hmac_sha256_wrapper,
    .generate_uuid = default_generate_uuid_wrapper,
    .fill_random   = default_fill_random_wrapper,
    .sha1          = default_sha1_wrapper,
    .bcrypt_hash   = default_bcrypt_hash_wrapper,
};

static inline csilk_crypto_driver_t* resolve_crypto(csilk_ctx_t* c) {
    return (c && c->crypto_driver) ? c->crypto_driver : &csilk_default_crypto_driver;
}
```

Update `_csilk_hmac_sha256`, `_csilk_generate_uuid`, `_csilk_fill_random`, `_csilk_sha1`, `_csilk_bcrypt_hash` to call `resolve_crypto(c)-><callback>(...)`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_crypto_driver && ctest --test-dir build -R test_crypto_driver --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/csilk/core/crypto.h src/crypto/crypto.c tests/security/test_crypto_driver.c
git commit -m "feat(crypto): ✨ add default crypto driver singleton and update dispatchers"
```

---

### Task 2: Bind Default Drivers in Server and Context Initialization

**Files:**
- Modify: `src/core/server/server_lifecycle.c`
- Modify: `src/core/uring/uring_server.c`
- Modify: `src/core/ctx/context.c`
- Test: `tests/core/test_utils_ext.c`

- [ ] **Step 1: Write failing test in `tests/core/test_utils_ext.c`**

Add test checking default driver binding on context:
```c
static void test_context_default_drivers(void) {
    printf("Testing context default drivers...\n");
    csilk_ctx_t c;
    _csilk_ctx_init(&c, NULL, NULL);

    assert(c.crypto_driver != NULL);
    assert(c.cipher_driver != NULL);
    assert(c.crypto_driver == &csilk_default_crypto_driver);

    printf("context default drivers passed!\n");
}
```
Add `test_context_default_drivers()` call to `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_utils_ext && ctest --test-dir build -R test_utils_ext --output-on-failure`  
Expected: Assertion fail (`c.crypto_driver != NULL` fails because it was NULL).

- [ ] **Step 3: Update `_csilk_ctx_init`, `csilk_server_init`, and `csilk_uring_server_init`**

In `src/core/ctx/context.c` inside `_csilk_ctx_init`:
```c
extern csilk_cipher_driver_t csilk_default_cipher_driver;

    c->crypto_driver = (s && s->crypto_driver) ? s->crypto_driver : &csilk_default_crypto_driver;
    c->cipher_driver = (s && s->cipher_driver) ? s->cipher_driver : &csilk_default_cipher_driver;
    c->storage_driver = s ? s->storage_driver : NULL;
```

In `src/core/server/server_lifecycle.c` inside `csilk_server_init`:
```c
    server->crypto_driver = &csilk_default_crypto_driver;
    server->cipher_driver = &csilk_default_cipher_driver;
```

In `src/core/uring/uring_server.c` inside `csilk_uring_server_init`:
```c
    server->crypto_driver = &csilk_default_crypto_driver;
    server->cipher_driver = &csilk_default_cipher_driver;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_utils_ext && ctest --test-dir build -R test_utils_ext --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/ctx/context.c src/core/server/server_lifecycle.c src/core/uring/uring_server.c tests/core/test_utils_ext.c
git commit -m "feat(core): ✨ bind default drivers in server and context initialization"
```

---

### Task 3: Add Context Driver Accessor Functions

**Files:**
- Modify: `include/csilk/core/context.h`
- Modify: `src/core/ctx/context.c`
- Test: `tests/core/test_utils_ext.c`

- [ ] **Step 1: Write failing test in `tests/core/test_utils_ext.c`**

Add test for driver getters:
```c
static void test_context_driver_getters(void) {
    printf("Testing context driver getters...\n");
    csilk_ctx_t c;
    _csilk_ctx_init(&c, NULL, NULL);

    csilk_crypto_driver_t* crypto = csilk_ctx_get_crypto_driver(&c);
    csilk_cipher_driver_t* cipher = csilk_ctx_get_cipher_driver(&c);
    csilk_storage_driver_t* storage = csilk_ctx_get_storage_driver(&c);

    assert(crypto == &csilk_default_crypto_driver);
    assert(cipher != NULL);
    assert(storage == NULL);

    printf("context driver getters passed!\n");
}
```
Call `test_context_driver_getters()` in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_utils_ext --output-on-failure`  
Expected: Build error (getters undeclared).

- [ ] **Step 3: Declare and implement getter functions**

In `include/csilk/core/context.h`:
```c
/** @brief Get the crypto driver bound to the context. */
csilk_crypto_driver_t* csilk_ctx_get_crypto_driver(const csilk_ctx_t* c);

/** @brief Get the cipher driver bound to the context. */
csilk_cipher_driver_t* csilk_ctx_get_cipher_driver(const csilk_ctx_t* c);

/** @brief Get the storage driver bound to the context. */
csilk_storage_driver_t* csilk_ctx_get_storage_driver(const csilk_ctx_t* c);
```

In `src/core/ctx/context.c`:
```c
csilk_crypto_driver_t*
csilk_ctx_get_crypto_driver(const csilk_ctx_t* c)
{
    return c ? c->crypto_driver : &csilk_default_crypto_driver;
}

csilk_cipher_driver_t*
csilk_ctx_get_cipher_driver(const csilk_ctx_t* c)
{
    return c ? c->cipher_driver : &csilk_default_cipher_driver;
}

csilk_storage_driver_t*
csilk_ctx_get_storage_driver(const csilk_ctx_t* c)
{
    return c ? c->storage_driver : NULL;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_utils_ext && ctest --test-dir build -R test_utils_ext --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/csilk/core/context.h src/core/ctx/context.c tests/core/test_utils_ext.c
git commit -m "feat(core): ✨ add public driver getter accessors on csilk_ctx_t"
```

---

### Task 4: Format Check & Full Test Suite Verification

**Files:**
- None (Verification and formatting)

- [ ] **Step 1: Apply clang-format**

Run: `cmake --build build --target format`

- [ ] **Step 2: Verify format check**

Run: `cmake --build build --target check-format`  
Expected: Exit code 0 (all formatted).

- [ ] **Step 3: Run full unit test suite**

Run: `ctest --test-dir build -E test_integration --output-on-failure`  
Expected: 100% tests passed.

- [ ] **Step 4: Commit formatting updates if any**

```bash
git add -u
git commit -m "style(core): 🎨 apply clang-format for default driver updates" || true
```
