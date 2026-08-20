# Audit Fixup Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 3 issues identified in the Client Lifetime Audit — MQ heap-use-after-free (TSan critical), `_check_recycle` TOCTOU (defensive), and ASan sendfile test timing (test-only).

**Architecture:** Three independent, targeted fixes across 3 files. No architectural changes. ~10 lines of production code, ~5 lines of test code.

**Spec:** `docs/superpowers/specs/2026-08-20-fixup-audit-findings-design.md`

---

## File Map

| File | Change | Responsibility |
|------|--------|---------------|
| `src/messaging/mq_core.c` | Fix UAF | Move `free(mq->monitors)` into `on_mq_close` |
| `src/core/server/connection_close.c` | Defensive hardening | Add early-exit guard + state re-read in `_check_recycle` |
| `tests/core/test_sendfile_workers.c` | Test fix | Extend warmup under ASan |

---

## Chunk 1: MQ UAF Fix

### Task 1: Fix MQ monitors UAF

**Files:**
- Modify: `src/messaging/mq_core.c:196-261`

- [ ] **Step 1: Add `free(mq->monitors)` to `on_mq_close`**

In `src/messaging/mq_core.c`, locate the `on_mq_close` function (line 196). After the existing `free(mq->global_middlewares);` call (line 235) and before `free(mq);` (line 238), add:

```c
    free(mq->monitors);
```

The relevant section should become:
```c
    free(mq->global_middlewares);
    free(mq->monitors);    // ← ADD THIS LINE
    CSILK_LOG_I("MQ: Message queue closed and resource cleanup complete");
    free(mq);
```

- [ ] **Step 2: Remove duplicate `free(mq->monitors)` from `_csilk_mq_free`**

In `src/messaging/mq_core.c`, locate `_csilk_mq_free` (line 251). Remove line 260:

```c
    // REMOVE THIS LINE:
    free(mq->monitors);
```

The function should end with just the async close trigger:
```c
CSILK_INTERNAL void
_mq_free(csilk_mq_t* mq)
{
    if (!mq) {
        return;
    }
    if (!csilk_io_is_closing((csilk_io_handle_t*)&mq->async_handle)) {
        mq->async_handle.data = mq;
        csilk_io_close((csilk_io_handle_t*)&mq->async_handle, on_mq_close);
    }
}
```

- [ ] **Step 3: Build and verify no compile errors**

Run:
```bash
cmake --build build --target csilk -j$(nproc) 2>&1
```

Expected: clean build, no warnings about the modified file.

- [ ] **Step 4: Run TSan tests to verify UAF is gone**

Run:
```bash
ctest --test-dir build_tsan -R "test_core_concurrency_stress" --timeout 30 --output-on-failure 2>&1 | grep -E "ThreadSanitizer|PASSED|FAILED|passed|failed"
```

Expected: No ThreadSanitizer warnings about `mq_core.c:260` or `_csilk_mq_free`. The test should report all scenarios passed.

- [ ] **Step 5: Commit**

```bash
git add src/messaging/mq_core.c
git commit -m "fix(mq): 🐛 free monitors in on_mq_close to eliminate UAF"
```

---

## Chunk 2: _check_recycle TOCTOU Hardening

### Task 2: Add defensive guards to _check_recycle

**Files:**
- Modify: `src/core/server/connection_close.c:146-157`

- [ ] **Step 1: Rewrite `_csilk_client_check_recycle` with early-exit and re-read**

Replace the current function body in `src/core/server/connection_close.c` (lines 146-157):

```c
// BEFORE:
void
_mcsilk_client_check_recycle(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t st = client->state;
    if ((st == CSILK_CONN_CLOSING || st == CSILK_CONN_CLOSED) &&
        atomic_load(&client->ref_count) <= 0 && atomic_load(&client->pending_io) <= 0) {
        client_destroy(client);
    }
}

// AFTER:
void
_csilk_client_check_recycle(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t st = client->state;
    if (st != CSILK_CONN_CLOSING && st != CSILK_CONN_CLOSED) {
        return;
    }
    int ref = atomic_load(&client->ref_count);
    int pio = atomic_load(&client->pending_io);
    if (client->state == st && ref <= 0 && pio <= 0) {
        client_destroy(client);
    }
}
```

Key changes:
1. Early exit if state is neither CLOSING nor CLOSED — avoids unnecessary atomic loads
2. Re-read `client->state` after atomic loads — if state changed during the window, skip destroy

- [ ] **Step 2: Build to verify no compile errors**

Run:
```bash
cmake --build build --target csilk -j$(nproc) 2>&1
```

Expected: clean build.

- [ ] **Step 3: Run connection tests to verify no regression**

Run:
```bash
ctest --test-dir build -R "test_connection" --timeout 10 --output-on-failure 2>&1
```

Expected: `24 passed, 0 failed`.

- [ ] **Step 4: Run TSan connection tests**

Run:
```bash
ctest --test-dir build_tsan -R "test_connection" --timeout 10 --output-on-failure 2>&1
```

Expected: `24 passed, 0 failed`, no TSan warnings.

- [ ] **Step 5: Commit**

```bash
git add src/core/server/connection_close.c
git commit -m "refactor(server): ♻️ add defensive guards to _check_recycle"
```

---

## Chunk 3: ASan Sendfile Test Timing

### Task 3: Extend warmup under ASan

**Files:**
- Modify: `tests/core/test_sendfile_workers.c:217`

- [ ] **Step 1: Add ASan-aware warmup**

In `tests/core/test_sendfile_workers.c`, locate the warmup line (line 217):

```c
    usleep(50000); /* 50ms warm-up */
```

Replace with:
```c
#if defined(__SANITIZE_ADDRESS__)
    usleep(200000); /* 200ms warmup under ASan (~4x slowdown) */
#else
    usleep(50000);  /* 50ms warmup normal */
#endif
```

- [ ] **Step 2: Build and run ASan sendfile test**

First build the ASan version:
```bash
cmake --build build_asan --target test_sendfile_workers -- -j$(nproc) 2>&1 | tail -3
```

Then run:
```bash
ASAN_OPTIONS="detect_leaks=0" timeout 60 ./build_asan/test_sendfile_workers 2>&1
```

Expected: All 4 worker configurations pass (1/2/4/8 workers), "All Multi-Worker Sendfile Tests Passed Successfully!"

- [ ] **Step 3: Run full ASan test suite for the affected test**

Run:
```bash
ctest --test-dir build_asan -R "test_sendfile_workers" --timeout 60 --output-on-failure 2>&1
```

Expected: PASS.

- [ ] **Step 4: Run Release sendfile test to confirm no regression**

Run:
```bash
ctest --test-dir build -R "test_sendfile_workers" --timeout 60 --output-on-failure 2>&1
```

Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
cmake --build build --target format 2>&1 | tail -3
git add tests/core/test_sendfile_workers.c
git commit -m "test(sendfile): ✅ extend warmup under ASan to prevent flaky timing"
```

---

## Final Verification

- [ ] **Run full TSan test suite**

```bash
ctest --test-dir build_tsan -E test_integration --timeout 30 --output-on-failure 2>&1 | tail -15
```

Expected: 100% pass, no TSan warnings related to mq_core.c.

- [ ] **Run full ASan test suite (key tests)**

```bash
ctest --test-dir build_asan -E test_integration --timeout 30 --output-on-failure 2>&1 | tail -15
```

Expected: 100% pass.

- [ ] **Run release test suite**

```bash
ctest --test-dir build -E test_integration --timeout 30 --output-on-failure 2>&1 | tail -15
```

Expected: All pass.

- [ ] **Run format check**

```bash
cmake --build build --target check-format 2>&1 | tail -5
```

Expected: No formatting issues.

- [ ] **Final commit message for review**

```bash
git log --oneline -5
```

Expected to see:
```
<hash> test(sendfile): ✅ extend warmup under ASan to prevent flaky timing
<hash> refactor(server): ♻️ add defensive guards to _check_recycle
<hash> fix(mq): 🐛 free monitors in on_mq_close to eliminate UAF
```
