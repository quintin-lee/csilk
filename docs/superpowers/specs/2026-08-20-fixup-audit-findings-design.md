# Audit Fixup — Client Lifetime + MQ UAF + ASan Sendfile

**Date:** 2026-08-20
**Source:** Client Lifetime Audit (2026-08-20), TSan/ASan stress runs
**Scope:** `src/messaging/mq_core.c`, `src/core/server/connection_close.c`, `tests/core/test_sendfile_workers.c`
**Philosophy:** Minimal, targeted fixes. No architectural changes. ~10 lines total.

---

## Problem 1: MQ UAF (Critical — TSan reports 6 instances)

### Root Cause

`on_mq_close` (mq_core.c:196) frees the MQ instance via `free(mq)` at line 238, but **never frees `mq->monitors`**. `_csilk_mq_free` (line 251) then calls `free(mq->monitors)` at line 260 — but if `on_mq_close` has already run asynchronously, `mq` itself is already freed, making the `monitors` access a use-after-free.

Call chain in TSan:
```
_csilk_mq_free → csilk_io_close → on_mq_close → free(mq)   [freed]
                 (returns to _csilk_mq_free)
_csilk_mq_free → free(mq->monitors)                         [UAF: mq is freed]
```

This fires in 6 of the 15 concurrency stress scenarios because each creates a server with an MQ, exercises it, then calls `csilk_server_free`.

### Fix

Move `free(mq->monitors)` from `_csilk_mq_free` into `on_mq_close`, after the other resource cleanup and before `free(mq)`.

```c
// mq_core.c — on_mq_close: add monitors free before mq free
static void on_mq_close(csilk_io_handle_t* handle) {
    csilk_mq_t* mq = (csilk_mq_t*)handle->data;
    if (!mq) return;
    // ... existing cleanup (wal, msgs, topics, middlewares) ...
    free(mq->global_middlewares);
    free(mq->monitors);   // ← ADD: was missing, causing UAF when _csilk_mq_free runs after
    CSILK_LOG_I("MQ: Message queue closed and resource cleanup complete");
    free(mq);
}

// mq_core.c — _csilk_mq_free: remove duplicate free
CSILK_INTERNAL void _csilk_mq_free(csilk_mq_t* mq) {
    if (!mq) return;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&mq->async_handle)) {
        mq->async_handle.data = mq;
        csilk_io_close((csilk_io_handle_t*)&mq->async_handle, on_mq_close);
    }
    // free(mq->monitors) REMOVED: now handled in on_mq_close
}
```

**Safety:** `on_mq_close` runs synchronously on the event loop thread before returning control. Even when called synchronously (non-async path), `monitors` is freed exactly once, then `mq` is freed. The async path also guarantees `on_mq_close` runs before the close callback returns.

**Risk:** None. Pure bug fix — a `free()` call was missing from the only teardown path that frees the struct.

---

## Problem 2: `_check_recycle` TOCTOU (Low — defensive)

### Root Cause

`connection_close.c:152-155` reads `state` non-atomically, then reads `ref_count` and `pending_io` atomically. In between these reads, another operation on the same worker thread could theoretically change `state` back to an active state (e.g., if a future feature adds off-thread operations that can flip state). The current single-thread model makes this impossible, but the guard is absent.

### Fix

Add an early-exit guard and a re-read check after the atomic loads:

```c
// connection_close.c — _csilk_client_check_recycle
void _csilk_client_check_recycle(csilk_client_t* client) {
    if (!client) return;
    csilk_conn_state_t st = client->state;
    if (st != CSILK_CONN_CLOSING && st != CSILK_CONN_CLOSED) {
        return;  // Early exit: not in a terminal state
    }
    int ref = atomic_load(&client->ref_count);
    int pio = atomic_load(&client->pending_io);
    if (client->state == st && ref <= 0 && pio <= 0) {
        client_destroy(client);
    }
}
```

**Changes:**
- Early exit if `state` is neither CLOSING nor CLOSED (prevents unnecessary work)
- Re-read `state` after atomic loads: if state changed during the window, skip destroy
- No change to the happy path when state is stable

**Risk:** Extremely low. Adds one extra state read per recycle check. In the common case where state is stable (READING/PROCESSING/WRITING), the early exit returns immediately — actually slightly faster than before because we skip the atomic loads entirely.

---

## Problem 3: ASan sendfile test timing (Low — test only)

### Root Cause

`test_sendfile_workers.c:217` waits 50ms warmup before firing requests. Under ASan instrumentation, the server runs ~2-3× slower, and the 8-worker configuration (40 concurrent clients × 5 requests each) cannot complete within 50ms.

### Fix

Detect ASan at compile time and extend warmup:

```c
// test_sendfile_workers.c — run_sendfile_worker_test
#if defined(__SANITIZE_ADDRESS__)
    usleep(200000); /* 200ms warmup under ASan (~4x slowdown) */
#else
    usleep(50000);  /* 50ms warmup in normal builds */
#endif
```

**Risk:** None. Pure test-only change. No production code touched.

---

## File Change Summary

| File | Lines Changed | Type |
|------|--------------|------|
| `src/messaging/mq_core.c` | +1, -1 | Bug fix |
| `src/core/server/connection_close.c` | +4 | Defensive hardening |
| `tests/core/test_sendfile_workers.c` | +5 | Test fix |
| **Total** | **~10 lines** | |

## Verification Plan

1. **TSan libuv:** `ctest --test-dir build_tsan -E test_integration` — all 197 tests pass, no new warnings
2. **TSan io_uring:** Same — MQ UAF warnings eliminated
3. **ASan libuv:** `ctest --test-dir build_asan -R test_sendfile_workers` — passes (was failing)
4. **Release libuv:** `ctest --test-dir build -E test_integration` — no regression
5. **Release io_uring:** Same — no regression
6. `cmake --build build --target format` — confirm clang-format compliance
7. `cmake --build build --target tidy` — confirm clang-tidy passes

## Out of Scope

- No changes to MQ architecture or shutdown ordering
- No changes to `state` field declaration (remains non-atomic for single-thread confinement)
- No changes to client lifetime semantics
- No changes to TSan suppression files (fix is at source, not suppression)
