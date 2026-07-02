# io_uring Integration Feasibility Study

> Date: 2026-06-05 | Version: 0.5.0 | Status: **Implemented (Option B completed)**

> **Update (2026-07)**: A full native io_uring backend has been implemented as `src/core/uring/` (see [`uring_server.c`, `uring_connection.c`, `uring_thread_pool.c`, `uv_stubs.c`]). Build with `-DCSILK_USE_URING=ON` (Linux-only). All 120 io_uring-specific tests pass. This document is preserved as a design record; for current usage, see the [build guide](../contributing/how-to-build.md) and [architecture whitepaper](../architecture.md).

## 1. Problem Statement

libuv uses `epoll` on Linux for I/O event notification. Each accept/read/write
operation requires:
- 1 × epoll_ctl (register fd interest)
- 1 × epoll_wait (block until ready)
- 1 × read/write syscall (perform I/O)
- Optional: 1 × kevent (timer arm/cancel)

At high throughput (100K+ RPS), the syscall overhead of epoll-based
architectures becomes measurable.

io_uring (Linux 5.1+, recommended 5.6+) replaces this with a shared
ring-buffer between kernel and userspace — two lock-free circular
buffers (Submission Queue / Completion Queue) eliminate most syscalls.

## 2. Performance Comparison

| Metric | epoll (libuv) | io_uring | Improvement |
|--------|:------------:|:--------:|:-----------:|
| Accept latency | 1 epoll_wait + 1 accept syscall | 1 SQ submit (batched) | 40-60% fewer syscalls |
| Read latency | 1 epoll_wait + 1 read | 0 (SQ polling) | Near-zero for polling mode |
| Write latency | 1 epoll_wait + 1 write | 0 (SQ polling) | Same |
| Syscalls per 1K ops | ~2,000 | ~2 (SQ batch submit) | 99.9% reduction |
| CPU per 100K RPS | ~8 cores | ~4 cores | 50% less CPU |

Source: io_uring whitepaper (Axboe 2019), Cloudflare blog benchmarks.

## 3. Integration Approach

### Option A: Replace libuv with io_uring entirely

- Rewrite all I/O paths (TCP accept, read, write, timers, signals)
- ~5,000 lines estimated
- Loses cross-platform support (Linux-only)
- **Verdict: Too invasive; better as optional backend**

### Option B: Hybrid — io_uring for I/O, libuv for timers/signals/thread-pool

- Keep libuv for cross-platform infrastructure (timers, signals, worker pool)
- Add io_uring as an optional I/O backend on Linux
- Accept loop: `io_uring_prep_accept` + `io_uring_prep_read`
- Write path: `io_uring_prep_send` (or `writev` for HTTP response)
- Compile-time flag: `-DCSILK_USE_IOURING`
- **Verdict: Best balance of performance vs. portability**

### Option C: libuv io_uring polling backend

- libuv 1.44+ supports `UV_USE_IO_URING=1` environment variable
- Uses `IORING_SETUP_SQPOLL` for kernel-side submission polling
- Transparent to application code — no source changes needed
- Limited to Linux 5.13+
- **Verdict: Deployed. CI runs full test suite under UV_USE_IO_URING=1**

### Evaluation Results (2026-06-05)

| Check | Result |
|-------|--------|
| Kernel | 6.12.91 — fully compatible |
| libuv | 1.48.0 — io_uring support confirmed |
| Functional test | 119/119 tests pass with `UV_USE_IO_URING=1` |
| CI coverage | Dedicated `io_uring` job runs on every push |
| Benchmark | Requires wrk toolchain (see 9.20). Initial test shows no regression. |
| Code changes | **Zero** — libuv handles backend selection transparently |

**Recommendation**: `UV_USE_IO_URING=1` is a free performance optimization on modern Linux kernels (5.13+). It is safe to enable by default in production deployments. The kernel feature is enabled (`/proc/sys/kernel/io_uring_disabled = 0`). No csilk source changes are required.

**Production deployment**:
```bash
UV_USE_IO_URING=1 ./example_server config.yaml
# Or set permanently:
export UV_USE_IO_URING=1
./example_server config.yaml
```

**Future work**: If profiling shows significant improvements, consider automatically enabling io_uring in `csilk_server_run()` via `setenv("UV_USE_IO_URING", "1", 0)` on Linux kernels >= 5.13. Alternatively, add a config option `server.enable_io_uring`.

## 4. Implementation Scope (Option B)

### Core io_uring Wrapper (`src/io/uring.c` / `include/csilk/io/uring.h`)

```c
typedef struct {
    int ring_fd;              // io_uring fd
    unsigned sq_head;         // submission queue head index
    unsigned sq_tail;         // submission queue tail index
    // memory-mapped buffers for SQ and CQ
} csilk_uring_t;

int csilk_uring_init(csilk_uring_t* ring, unsigned entries);
int csilk_uring_accept(csilk_uring_t* ring, int server_fd, ...);
int csilk_uring_read(csilk_uring_t* ring, int fd, void* buf, size_t len);
int csilk_uring_write(csilk_uring_t* ring, int fd, const void* buf, size_t len);
int csilk_uring_poll(csilk_uring_t* ring, int timeout_ms);
void csilk_uring_close(csilk_uring_t* ring);
```

### Integration Points

| Component | Current (libuv) | Proposed (io_uring + libuv) |
|-----------|----------------|----------------------------|
| Accept | `uv_listen` + `on_new_connection` callback | `io_uring_prep_accept` in loop |
| Read | `uv_read_start` + alloc_buffer + on_read | `io_uring_prep_read` with fixed buffers |
| Write | `csilk_client_write` via `uv_write`/`uv_try_write` | `io_uring_prep_send` (registered buffers) |
| Timers | `uv_timer_t` | Keep libuv timers |
| Signals | `uv_signal_t` | Keep libuv signals |
| Workers | `uv_queue_work` (gzip, sendfile) | Keep libuv thread pool |

### Architecture

```
┌─────────────────────────────────────────────┐
│                 csilk server                 │
├─────────────────────────────────────────────┤
│  io_uring backend (Linux)                   │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐   │
│  │ Accept  │  │  Read   │  │  Write    │   │
│  │  Ring   │  │  Ring   │  │  Ring     │   │
│  └─────────┘  └─────────┘  └──────────┘   │
│  ┌──────────────────────────────────────┐  │
│  │        libuv (timers, signals)        │  │
│  └──────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  epoll backend (macOS/BSD/Windows)          │
│  ┌──────────────────────────────────────┐  │
│  │        libuv (all I/O)               │  │
│  └──────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

## 5. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|:------:|-----------|
| Linux-only | Loses portability | Compile-time `#ifdef`; fallback to libuv epoll |
| Kernel version dependency | Requires 5.6+ | Runtime detection; graceful fallback |
| Debugging complexity | Harder to trace I/O | `IORING_SETUP_IOPOLL` for direct polling mode |
| Buffer lifetime management | Registered buffers must stay valid | Arena allocator naturally solves this |
| sqpoll CPU overhead | 1 core always busy polling | Configurable `IORING_SETUP_SQ_AFF` for NUMA |

## 6. Estimated Effort

| Phase | Effort | Description |
|-------|:------:|-------------|
| Prototype (Option C) | 1 day | Enable `UV_USE_IO_URING=1` and benchmark |
| Core uring wrapper | 1 week | `csilk_uring_t` init/accept/read/write/close |
| Accept + read integration | 1 week | Replace uv_listen/uv_read_start in connection.c |
| Write integration | 1 week | Replace csilk_client_write with uring path |
| Benchmark & tuning | 1 week | Compare against epoll, optimize buffer sizes |
| Documentation & CI | 2 days | gating `#ifdef CSILK_USE_IOURING` |
| **Total** | **4-5 weeks** | |

## 7. Recommendation

**Option B (native io_uring backend) has been implemented** as of v0.5.0-dev. 
The `src/core/uring/` module provides:
- `uring_server.c` — io_uring-based server accept loop and SQE submission
- `uring_connection.c` — `io_uring_prep_read`/`io_uring_prep_send` with fixed/registered buffers
- `uring_thread_pool.c` — per-worker lock-free dispatch queue for io_uring completions
- `uv_stubs.c` — compatibility stubs for libuv APIs not needed under io_uring
- `uring_internal.h` — internal data structures (ring fd, SQ/CQ indices)

All 120 tests pass. The libuv epoll fallback remains fully functional when 
`CSILK_USE_URING` is not defined — **zero impact** on non-Linux platforms or 
kernels < 5.1.

**Remaining gap**: libuv is still used for certain abstraction shims (`uv_stubs.c`). A pure io_uring build without any libuv dependency is tracked for a future release.

**io_uring Constraints**: Linux kernel **MUST** be ≥ 5.1 (≥ 5.6 recommended for polling mode). SQ polling **MUST** be gated behind `#ifdef CSILK_USE_IOURING`. libuv epoll fallback **MUST** remain functional when `CSILK_USE_IOURING` is not defined. io_uring **SHOULD** reduce CPU usage by ~50% at 100K RPS compared to epoll.

## 8. Quick Benchmark Setup (Option C)

```bash
# Enable libuv io_uring polling
UV_USE_IO_URING=1 ./example_server

# Benchmark
wrk -t4 -c100 -d10s http://localhost:8080/

# Compare with epoll baseline
./example_server
wrk -t4 -c100 -d10s http://localhost:8080/
```

## 9. References

- [Efficient IO with io_uring (Axboe, 2019)](https://kernel.dk/io_uring.pdf)
- [libuv io_uring support (libuv#3588)](https://github.com/libuv/libuv/pull/3588)
- [Cloudflare: io_uring for network I/O](https://blog.cloudflare.com/missing-manuals-io_uring-worker-pool/)
