# io_uring Completion Lifecycle — Stability Refactoring

**Date**: 2026-08-16  
**Status**: Implemented  
**Commit**: `12453092`

---

## 1. State Machine

### Handle Lifecycle
```
HANDLE_CREATED ──init/activate──▶ HANDLE_ACTIVE
                                  │
                                  ├──submit SQE──▶ SUBMITTED (kernel owns)
                                  │                │
                                  │                ▼ CQE arrives
                                  │         check: is_stale(handle, gen)?
                                  │           YES → DROP silently
                                  │           NO  → dispatch(op, res)
                                  │
                                  └──close()──▶ HANDLE_CLOSING
                                              gen++  (invalidates in-flight ops)
                                              fd=-1 / ACTIVE cleared
                                              ▼
                                         HANDLE_CLOSED
```

### Operation Dispatch Table

| Op | CQE res≥0 | CQE res=-ETIME | CQE res=-ECANCELED | CQE res<0 (other) |
|---|---|---|---|---|
| POLL_LISTEN | re-arm + call cb | skip | re-arm + call cb | skip |
| POLL_READ (event) | read() → cb | skip | skip (EOF) | map ECONNRESET/EPIPE/ETIMEDOUT→EOF |
| POLL_READ (nread==0) | — | — | — | EOF cb |
| UV_WRITE | cb(req, res) | — | — | cb(req, res) |
| TMR_GENERIC | cb(tmr) | **cb(tmr)** | **cb(tmr)** | skip |
| POLL_ASYNC | drain fd, re-arm, cb | — | — | skip |
| POLL_SIGNAL | drain fd, re-arm, cb | — | — | skip |

---

## 2. Bugs Found and Fixed

### BUG-1: Stale read after connection close
**Path**: `poll(POLLIN)` → connection closed (gen++ / CLOSING) → `read()` succeeds → stale callback  
**Fix**: Post-read generation guard — check `s->generation == gen && !CLOSING` before calling `read_cb`.

### BUG-2: Listen stale CQE
**Path**: listen poll fires after handle closed  
**Fix**: `is_stale_poll()` now guards POLL_LISTEN entry.

### BUG-3: Timer CLOSE race
**Path**: timer CQE arrives while handle is CLOSING  
**Fix**: Extracted `is_stale_timer()` — checks active, !closing, gen match in one place.

### BUG-5: ECONNRESET/EPIPE treated as generic errors
**Path**: `read()` returns -ECONNRESET → passed as raw errno to `on_read`  
**Fix**: Map `ECONNRESET`, `EPIPE`, `ETIMEDOUT` → UV_EOF (-4095) in both poll-error branch and post-read-error branch.

### BUG-6: Async/Signal stale CQE
**Path**: handle fd reused, old CQE triggers callback on wrong handle  
**Fix**: `is_stale_poll()` guards POLL_ASYNC and POLL_SIGNAL entry.

---

## 3. New Helpers in `uring_run.c`

```c
static int is_stale_poll(csilk_io_handle_t* handle, uint8_t gen);
static int is_stale_timer(csilk_io_timer_t* tmr, uint8_t gen);
```

Both check: non-null + fd>=0 + ACTIVE + !CLOSING + gen matches.

---

## 4. Ownership Model

| Resource | Owner | Freer |
|---|---|---|
| `csilk_client_t` | worker pool | `client_destroy()` → `pool_put()` |
| `csilk_io_write_t*` req | caller (stack/arena) | caller |
| `iov` (writev) | malloc in `csilk_io_write` | `csilk_uv_on_write_done` |
| `ctx[]` (write dispatch) | malloc in `csilk_io_write` | `csilk_uv_on_write_done` |
| Read buffer | arena pool | `pool_put_read_buf` in `on_read` |

**Invariant**: `async_ref` protects `client` from being freed while any write is in flight. Stale writes (gen mismatch) free their own resources without touching the new client.

---

## 5. Verification Results

| Test | libuv | io_uring | ASAN+uring |
|---|---|---|---|
| Unit tests (166) | 166/166 ✅ | 166/166 ✅ | 167/168 ⚠️ |
| ASAN failure | — | — | `test_mq_monitor` — pre-existing 32B MQ leak, unrelated |
| Stability (3×) | — | 3/3 ✅ | — |
| Format | ✅ | ✅ | ✅ |
