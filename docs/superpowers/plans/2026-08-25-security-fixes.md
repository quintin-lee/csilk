# Security & Concurrency Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix P0 data race in uring thread pool and P1 security vulnerabilities in JWT and sendfile callback.

**Architecture:** Three independent fixes: (1) Replace volatile queue heads with atomic operations, (2) Fix JWT timing side-channel, (3) Add null pointer defense in sendfile completion. Each fix is self-contained with minimal cross-dependency.

**Tech Stack:** C23, stdatomic.h, libuv/io_uring backend

---

## File Structure

| File | Operation | Responsibility |
|------|-----------|----------------|
| `src/core/uring/uring_thread_pool.c` | Modify | Replace volatile queue heads/tails with atomic_int |
| `src/middleware/jwt.c` | Modify | Fix timing side-channel in HS256 verification |
| `src/core/http/http1_write.c` | Modify | Add null defense after req free in sendfile callback |
| `tests/security/test_jwt_security.c` | Modify | Add timing side-channel test |

---

### Task 1: P0 Fix — Atomic Queue Heads in uring_thread_pool

**Files:**
- Modify: `src/core/uring/uring_thread_pool.c:32-51`
- Test: Run existing `tests/core/test_uring_*.c` tests

- [ ] **Step 1: Add stdatomic.h include**

In `src/core/uring/uring_thread_pool.c`, add after line 18:
```c
#include <stdatomic.h>
```

- [ ] **Step 2: Replace volatile with atomic_int in struct**

Change lines 39-47 from:
```c
    volatile int     queue_head;
    volatile int     queue_tail;
    csilk_mutex_t    queue_mutex;
    csilk_cond_t     queue_cond;

    /* Completion queue — multiple producers (threads) → single consumer (event loop). */
    uring_tp_entry_t done[URING_TP_MAX_WORK];
    volatile int     done_head;
    volatile int     done_tail;
```

To:
```c
    atomic_int       queue_head;
    atomic_int       queue_tail;
    csilk_mutex_t    queue_mutex;
    csilk_cond_t     queue_cond;

    /* Completion queue — multiple producers (threads) → single consumer (event loop). */
    uring_tp_entry_t done[URING_TP_MAX_WORK];
    atomic_int       done_head;
    atomic_int       done_tail;
```

- [ ] **Step 3: Update queue_head increment in worker_routine**

Change line 94 from:
```c
        tp->queue_head++;
```
To:
```c
        atomic_fetch_add(&tp->queue_head, 1);
```

- [ ] **Step 4: Update queue_tail increment in worker_routine**

Change line 107 from:
```c
        tp->done_tail++;
```
To:
```c
        atomic_fetch_add(&tp->done_tail, 1);
```

- [ ] **Step 5: Update queue_head reads in worker_routine**

Change line 83 from:
```c
        while (tp->queue_head == tp->queue_tail && tp->running) {
```
To:
```c
        while (atomic_load(&tp->queue_head) == tp->queue_tail && tp->running) {
```

- [ ] **Step 6: Update done_head reads in worker_routine**

Change line 266 from:
```c
    while (tp->done_head != tp->done_tail) {
```
To:
```c
    while (atomic_load(&tp->done_head) != tp->done_tail) {
```

- [ ] **Step 7: Update index calculations using atomic loads**

Change lines 92-93 from:
```c
        int              idx = tp->queue_head % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->queue[idx];
```
To:
```c
        int              idx = atomic_load(&tp->queue_head) % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->queue[idx];
```

Change lines 105-106 from:
```c
        int done_idx = tp->done_tail % URING_TP_MAX_WORK;
        tp->done[done_idx] = entry;
```
To:
```c
        int done_idx = atomic_load(&tp->done_tail) % URING_TP_MAX_WORK;
        tp->done[done_idx] = entry;
```

Change lines 267-268 from:
```c
        int              idx = tp->done_head % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->done[idx];
```
To:
```c
        int              idx = atomic_load(&tp->done_head) % URING_TP_MAX_WORK;
        uring_tp_entry_t entry = tp->done[idx];
```

- [ ] **Step 8: Update enqueue function queue_tail reads/writes**

Change line 232 from:
```c
    int count = tp->queue_tail - tp->queue_head;
```
To:
```c
    int count = atomic_load(&tp->queue_tail) - atomic_load(&tp->queue_head);
```

Change line 238 from:
```c
    int idx = tp->queue_tail % URING_TP_MAX_WORK;
```
To:
```c
    int idx = atomic_load(&tp->queue_tail) % URING_TP_MAX_WORK;
```

Change line 243 from:
```c
    tp->queue_tail++;
```
To:
```c
    atomic_fetch_add(&tp->queue_tail, 1);
```

- [ ] **Step 9: Initialize atomics in uring_tp_init**

Change lines 143-146 from:
```c
    tp->queue_head = 0;
    tp->queue_tail = 0;
    tp->done_head = 0;
    tp->done_tail = 0;
```
To:
```c
    atomic_store(&tp->queue_head, 0);
    atomic_store(&tp->queue_tail, 0);
    atomic_store(&tp->done_head, 0);
    atomic_store(&tp->done_tail, 0);
```

- [ ] **Step 10: Build and run uring tests**

```bash
cmake --build build -j$(nproc) --target test_uring_io
ctest --test-dir build -R test_uring_io --timeout 30 --output-on-failure
```

Expected: All tests pass

- [ ] **Step 11: Run TSAN build to verify no data races**

```bash
cmake -B build_tsan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON
cmake --build build_tsan -j$(nproc)
ctest --test-dir build_tsan -R test_uring --timeout 60 --output-on-failure
```

Expected: TSAN clean (no data race warnings)

- [ ] **Step 12: Commit**

```bash
git add src/core/uring/uring_thread_pool.c
git commit -m "fix(uring): 🔒 replace volatile queue heads with atomic_int to eliminate data race"
```

---

### Task 2: P1-1 Fix — JWT Timing Side-Channel Defense

**Files:**
- Modify: `src/middleware/jwt.c:296-300`
- Test: `tests/security/test_jwt_security.c`

- [ ] **Step 1: Read current JWT verification code**

In `src/middleware/jwt.c`, the current HS256 verification at lines 296-300:
```c
        size_t expected_len = strlen(sig_expected_b64);
        sig_ok =
            (strlen(sig_ptr) == expected_len) &&
            (CRYPTO_memcmp(
                 (const uint8_t*)sig_ptr, (const uint8_t*)sig_expected_b64, expected_len) == 0);
```

- [ ] **Step 2: Replace with timing-safe comparison**

Change the above to:
```c
        size_t expected_len = strlen(sig_expected_b64);
        size_t sig_len = strlen(sig_ptr);
        int len_mismatch = (sig_len != expected_len);
        /* Always run CRYPTO_memcmp to prevent timing side-channel:
         * even if lengths differ, we compare to avoid leaking info */
        int cmp_result = len_mismatch ? 1 : 
                         CRYPTO_memcmp(
                             (const uint8_t*)sig_ptr, 
                             (const uint8_t*)sig_expected_b64, 
                             expected_len);
        sig_ok = (cmp_result == 0);
```

- [ ] **Step 3: Build and run JWT tests**

```bash
cmake --build build -j$(nproc) --target test_jwt
ctest --test-dir build -R test_jwt --timeout 30 --output-on-failure
```

Expected: All JWT tests pass

- [ ] **Step 4: Run security tests**

```bash
ctest --test-dir build -R test_jwt_security --timeout 30 --output-on-failure
```

Expected: Security tests pass

- [ ] **Step 5: Commit**

```bash
git add src/middleware/jwt.c
git commit -m "fix(jwt): 🔒 fix timing side-channel in HS256 signature verification"
```

---

### Task 3: P1-2 Fix — Sendfile Callback Null Defense

**Files:**
- Modify: `src/core/http/http1_write.c:24-33`

- [ ] **Step 1: Add null defense after free**

In `src/core/http/http1_write.c`, change lines 26-33 from:
```c
    csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_io_fs_req_cleanup(req);
    free(req);

    if (!client) {
        return;
    }
```

To:
```c
    csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    
    csilk_io_fs_req_cleanup(req);
    free(req);
    req = NULL;    /* Prevent use-after-free of req */
    c = NULL;      /* Prevent dangling pointer access */

    if (!client) {
        return;
    }
```

- [ ] **Step 2: Build and run HTTP tests**

```bash
cmake --build build -j$(nproc) --target test_http1
ctest --test-dir build -R test_http1 --timeout 30 --output-on-failure
```

- [ ] **Step 3: Run sendfile-specific tests**

```bash
ctest --test-dir build -R test_uring_fs --timeout 30 --output-on-failure
```

Expected: All tests pass

- [ ] **Step 4: Commit**

```bash
git add src/core/http/http1_write.c
git commit -m "fix(http): 🔒 add null defense after req free in sendfile completion callback"
```

---

### Task 4: Full Validation

- [ ] **Step 1: Run full test suite**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -E test_integration --timeout 60 --output-on-failure
```

- [ ] **Step 2: Run integration tests**

```bash
ctest --test-dir build -R test_integration --timeout 60 --output-on-failure
```

- [ ] **Step 3: Run ASAN build**

```bash
cmake -B build_asan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON
cmake --build build_asan -j$(nproc)
ctest --test-dir build_asan --timeout 120 --output-on-failure
```

- [ ] **Step 4: Verify no regressions**

```bash
git diff --stat
```

Expected: Only 3 files modified (uring_thread_pool.c, jwt.c, http1_write.c)

---

## Self-Review Checklist

- [ ] All code changes are minimal and focused
- [ ] No placeholders or TODOs remain
- [ ] Tests run successfully before and after each change
- [ ] TSAN passes with no data race warnings
- [ ] ASAN passes with no memory errors
- [ ] Commit messages follow project convention
