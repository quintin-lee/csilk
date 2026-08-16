# io_uring Completion Lifecycle — Stability Refactoring

**Date**: 2026-08-16  
**Status**: Implementation

## 1. Current State Machine

### Handle Lifecycle
```
HANDLE_CREATED ──init/activate──▶ HANDLE_ACTIVE
                                  │
                                  ├──poll_add──▶ SUBMITTED (kernel owns SQE)
                                  │                │
                                  │                ▼
                                  │           CQE arrives
                                  │                │
                                  │         ┌──────┴───────┐
                                  │         │ stale?        │
                                  │         │ gen != stored? │
                                  │         └──────┬───────┘
                                  │                │ NO
                                  │                ▼
                                  │           dispatch(op, ptr, gen, res)
                                  │
                                  └──close──▶ HANDLE_CLOSING
                                              │
                                          fd=-1 / gen++
                                              │
                                              ▼
                                         HANDLE_CLOSED
```

### Operation States per CQE Type

| Op | CQE res≥0 | CQE res=-ETIME | CQE res=-ECANCELED | CQE res<0 (other) |
|---|---|---|---|---|
| POLL_LISTEN | re-arm + call cb | **skip** (not expected) | re-arm + call cb | skip |
| POLL_READ (event) | read() into buf | **skip** (not expected) | skip (EOF from read) | error cb |
| POLL_READ (data) | **call cb directly** | - | - | - |
| POLL_READ (EOF) | - | - | - | nread=0 → EOF cb |
| UV_WRITE | cb(req, res) | - | - | cb(req, res) |
| TMR_GENERIC | cb(tmr) | **cb(tmr)** | **cb(tmr)** (cancel succeeded) | skip |
| POLL_ASYNC | drain fd, re-arm, cb | - | - | skip |
| POLL_SIGNAL | drain fd, re-arm, cb | - | - | skip |

## 2. Bugs Found

### BUG-1: Stale read after connection close
**File**: `uring_run.c` line 129  
**Path**: `read(s->fd, ...)` succeeds → `s->read_cb(s, nread, &buf)` called  
**Problem**: Between the kernel reporting POLLIN and us calling read_cb, the connection may have been closed (another thread/path calls `csilk_io_close`). The `generation` check at line 118 only guards the *entry* to the branch, not the actual callback.  
**Fix**: Add `s->generation == gen` check before calling `read_cb` for nread > 0.

### BUG-2: Listen stale CQE
**File**: `uring_run.c` lines 99-113  
**Path**: listen poll fires, stream was closed concurrently  
**Problem**: Check `(s->flags & CSILK_IO_HANDLE_ACTIVE) && !(s->flags & CSILK_IO_HANDLE_CLOSING)` is correct but missing `gen` check (listen polls don't encode gen).  
**Fix**: Add `s->generation` comparison using handle->generation.

### BUG-3: Timer CLOSE race
**File**: `uring_run.c` lines 231-253  
**Path**: timer CQE arrives while handle is CLOSING (close was called after cancel SQE submitted)  
**Problem**: Current code checks `!(tmr->flags & CSILK_IO_HANDLE_CLOSING)` but if close increments generation *and* sets CLOSING before the CQE is processed, we need both checks. Also `-ETIME` and `-ECANCELED` must both be accepted.  
**Fix**: Keep generation check, keep `-ETIME`/`-ECANCELED` acceptance, add `gen == tmr->generation` explicit check.

### BUG-4: Write lifetime leak
**File**: `uring_write.c` lines 44-45  
**Path**: `req->cb` and `req->handle` are stored but `req->data` never checked for free  
**Problem**: If write fails at submission (sqe==NULL), the request struct is never freed.  
**Fix**: This is already handled by the caller — req is stack-allocated in most paths. No change needed.

### BUG-5: No standardized error mapping for connection errors
**File**: `uring_run.c` line 119, connection_io.c line 301  
**Path**: `read()` returns -ECONNRESET or -EPIPE  
**Problem**: These should be treated as connection termination (same as EOF), but are passed through as raw errno.  
**Fix**: In the read error branch, map `-ECONNRESET` and `-EPIPE` to `-4095` (UV_EOF equivalent).

### BUG-6: Poll async/signal stale CQE
**File**: `uring_run.c` lines 185-222  
**Problem**: No generation check for async and signal polls. If the handle is reused, old CQEs could trigger callbacks.  
**Fix**: Add generation checks (async has `async->generation`, signal has `sig->generation`).

## 3. Fixes to Apply

### Fix A: `uring_run.c` — Read success path stale guard
Add `s->generation == gen` before calling `s->read_cb(s, nread, &buf)` for nread > 0.

### Fix B: `uring_run.c` — Listen generation check
Add `s->generation` comparison (using a saved generation or current).

### Fix C: `uring_run.c` — Timer: explicit gen check + -ETIME/-ECANCELED
Normalize to use `gen == tmr->generation`.

### Fix D: `uring_run.c` — Read error: map ECONNRESET/EPIPE to UV_EOF
In the error branch, check for these specific errno values.

### Fix E: `uring_run.c` — Async/signal: add generation check
Add `async->generation == gen` and `sig->generation == gen` checks.

### Fix F: `uring_internal.h` — Add UV_EOF constant alias
Define `CSILK_IO_EOF` (-4095) as the canonical EOF error code.

## 4. Verification Plan

1. Normal TCP accept/read/write/close cycle
2. Timer fires normally, timer cancelled then CQE arrives
3. Connection closed, old CQE arrives (stale detection)
4. Handle reused, old CQE arrives (generation mismatch)
5. Client reset/timeout/EOF
6. ASAN + UBSAN run
7. High concurrency stress test
