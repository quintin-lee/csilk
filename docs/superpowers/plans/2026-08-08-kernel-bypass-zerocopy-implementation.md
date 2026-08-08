# Kernel-Bypass & Zero-Copy Performance Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement AF_XDP Zero-Copy UMEM hardware bypass, io_uring SQPOLL zero-syscall kernel polling, String View HTTP slice parsing, and vector scatter-gather I/O pipelines with seamless fallback in `csilk` (server-c).

**Architecture:** Add AF_XDP UMEM management under `src/core/io/`, io_uring SQPOLL under `src/core/uring/`, zero-copy HTTP slice parser under `src/core/http/`, and public header `include/csilk/core/io_perf.h`.

**Tech Stack:** C23, Linux eBPF/AF_XDP (libxdp/libbpf), io_uring, CMake, clang-format, clang-tidy

**Spec:** `docs/superpowers/specs/2026-08-08-kernel-bypass-zerocopy-design.md`

---

## File Structure

### New Files to Create

| File | Responsibility |
|------|---------------|
| `include/csilk/core/io_perf.h` | Public API header for performance probe, XDP, and SQPOLL controls |
| `src/core/io/af_xdp_internal.h` | Internal XSK Ring buffers and DMA descriptor structures |
| `src/core/io/af_xdp_zerocopy.c` | AF_XDP UMEM pool allocation & Fill/Rx/Tx/Completion ring management |
| `src/core/uring/uring_sqpoll.c` | io_uring `IORING_SETUP_SQPOLL` & `IOPOLL` kernel polling loop |
| `src/core/uring/uring_vector.c` | Vector Scatter-Gather I/O (`writev` & `io_uring_prep_writev2`) |
| `src/core/http/http1_zerocopy.c` | Zero-copy String View / Memory Slice HTTP header parser |
| `src/core/io/io_perf_probe.c` | Kernel capability probing & graceful fallback logic |
| `tests/core/test_af_xdp_zerocopy.c` | Unit tests for UMEM pool and XSK rings |
| `tests/core/test_uring_sqpoll.c` | Unit tests for io_uring SQPOLL mode and wakeup logic |
| `tests/core/test_http1_zerocopy.c` | Unit tests for zero-copy slice HTTP parsing |
| `tests/core/test_io_perf_fallback.c` | Test capability probe and fallback under restricted environments |

### Files to Modify

| File | Change |
|------|--------|
| `cmake/sources.cmake` | Add new IO performance source files |
| `cmake/tests.cmake` | Register new test targets |

---

### Task 1: Create Public Header & Internal Definitions

**Files:**
- Create: `include/csilk/core/io_perf.h`, `src/core/io/af_xdp_internal.h`

- [ ] **Step 1: Create `include/csilk/core/io_perf.h`**
Define `csilk_io_mode_t`, `csilk_io_perf_info_t`, `csilk_io_perf_probe`, `csilk_io_perf_enable_xdp`, and `csilk_io_perf_enable_sqpoll`.

- [ ] **Step 2: Create `src/core/io/af_xdp_internal.h`**
Define `csilk_xdp_umem_pool_t`, XSK ring buffer descriptors, and internal frame helpers.

---

### Task 2: Implement Zero-Copy String View HTTP Parser (`http1_zerocopy.c`)

**Files:**
- Create: `src/core/http/http1_zerocopy.c`
- Create: `tests/core/test_http1_zerocopy.c`

- [ ] **Step 1: Implement `http1_zerocopy.c`**
Implement `csilk_str_view_t` and `csilk_header_slice_t` slice parsing without dynamic allocations.

- [ ] **Step 2: Write unit test `test_http1_zerocopy.c`**
Verify zero-copy slice parsing for request headers and URLs.

---

### Task 3: Implement AF_XDP Zero-Copy UMEM Pool & Rings (`af_xdp_zerocopy.c`)

**Files:**
- Create: `src/core/io/af_xdp_zerocopy.c`
- Create: `tests/core/test_af_xdp_zerocopy.c`

- [ ] **Step 1: Implement `af_xdp_zerocopy.c`**
UMEM hugepage allocation, XSK Fill/Rx/Tx/Completion ring updates, and `csilk_ctx_from_xdp_frame()`.

- [ ] **Step 2: Write unit test `test_af_xdp_zerocopy.c`**
Verify UMEM memory alignment and ring index math.

---

### Task 4: Implement io_uring Kernel SQPOLL & IOPOLL Zero-Syscall Mode (`uring_sqpoll.c`)

**Files:**
- Create: `src/core/uring/uring_sqpoll.c`
- Create: `tests/core/test_uring_sqpoll.c`

- [ ] **Step 1: Implement `uring_sqpoll.c`**
Configure `IORING_SETUP_SQPOLL` & `IORING_SETUP_IOPOLL`, handle `IORING_SQ_NEED_WAKEUP`, and file registration (`IORING_REGISTER_FILES`).

- [ ] **Step 2: Write unit test `test_uring_sqpoll.c`**
Verify SQPOLL initialization and kernel thread wakeup behavior.

---

### Task 5: Implement Vector Scatter-Gather Response Pipeline (`uring_vector.c`)

**Files:**
- Create: `src/core/uring/uring_vector.c`

- [ ] **Step 1: Implement `uring_vector.c`**
Scatter-gather `io_uring_prep_writev2()` pipeline for HTTP status line, headers, and body payloads.

---

### Task 6: Implement Environment Capability Probe & Graceful Fallback (`io_perf_probe.c`)

**Files:**
- Create: `src/core/io/io_perf_probe.c`
- Create: `tests/core/test_io_perf_fallback.c`

- [ ] **Step 1: Implement `io_perf_probe.c`**
Inspect kernel version (>= 5.4), `CAP_SYS_ADMIN`/`CAP_NET_RAW` capabilities, and execute smooth fallback chain (XDP -> SQPOLL -> io_uring -> libuv).

- [ ] **Step 2: Write test `test_io_perf_fallback.c`**
Verify fallback logic under simulated capability absence.

---

### Task 7: CMake Integration & Final Verification

**Files:**
- Modify: `cmake/sources.cmake`, `cmake/tests.cmake`

- [ ] **Step 1: Add new sources to `cmake/sources.cmake` and `cmake/tests.cmake`**
- [ ] **Step 2: Full clean rebuild and test execution**
Run `make check-format` and `ctest --output-on-failure`.
