# Kernel-Bypass & Zero-Copy Performance Engine Design Specification

## Overview

This specification defines the architecture, components, API contracts, and implementation details for integrating **AF_XDP (XSK) Zero-Copy UMEM Hardware Bypass**, **io_uring Kernel SQPOLL/IOPOLL Zero-Syscall Polling**, and a **Zero-Copy Scatter-Gather Vector I/O Pipeline** into the `csilk` (server-c) web framework.

The goal is to enable `csilk` to reach ultra-high throughput (10M+ RPS capable) and ultra-low P99.9 latency under Linux environments with zero-copy packet processing, zero-syscall steady-state submission, and zero-allocation HTTP header parsing, while providing 100% reliable fallback to standard `io_uring` or `libuv` event loops when kernel or privilege constraints exist.

---

## 1. System Architecture & Module Boundaries

### 1.1 Directory Structure

```
include/csilk/
  └── core/
      └── io_perf.h             # Public I/O performance engine interfaces

src/
  └── core/
      ├── io/
      │   ├── af_xdp_zerocopy.c  # AF_XDP UMEM pool & XSK Fill/Rx/Tx/Completion ring
      │   └── af_xdp_internal.h  # Internal XSK Ring buffers and DMA descriptor structs
      ├── uring/
      │   ├── uring_sqpoll.c     # IORING_SETUP_SQPOLL & IOPOLL kernel polling loop
      │   └── uring_vector.c     # Vector Scatter-Gather I/O & writev zero-copy pipeline
      └── http/
          └── http1_zerocopy.c   # String View / Memory Slice zero-copy HTTP header parser
```

### 1.2 Architectural Principles & Safety Guarantees

1. **Zero Allocation & Zero Copy**: Packets received via AF_XDP Rx Ring reside in aligned UMEM hugepage frames mapped directly to DMA descriptors. HTTP parsing operates on `csilk_str_view_t` slices without allocating dynamic memory.
2. **Zero Syscall Steady-State**: Under `IORING_SETUP_SQPOLL`, submission queue entries (SQEs) are processed asynchronously by a dedicated kernel thread without invoking `io_uring_enter()` system calls during steady-state I/O.
3. **Seamless Graceful Fallback**: Probe functions inspect kernel versions (>= 5.4), `CAP_SYS_ADMIN`/`CAP_NET_RAW` privileges, and hardware NIC capabilities. If AF_XDP or SQPOLL initialization fails, `csilk` automatically falls back to standard `io_uring` or `libuv` mode without application failure or panic.

---

## 2. AF_XDP Zero-Copy UMEM Pool & XSK Ring Buffers

### 2.1 UMEM Memory Pool Layout (`af_xdp_zerocopy.c`)

Memory is allocated from page-aligned 2MB/1GB hugepages and registered with Linux eBPF/AF_XDP subsystem via `xsk_umem__create()`:

```c
typedef struct {
    void*                buffer;         /* Base pointer to aligned hugepage block */
    size_t               size;           /* Total size (e.g., 64MB) */
    uint32_t             frame_size;     /* Frame size (2048 or 4096 bytes) */
    uint32_t             frame_count;    /* Total frames */
    struct xsk_umem_info *umem;          /* libxdp / libbpf xsk_umem handle */
} csilk_xdp_umem_pool_t;
```

### 2.2 XSK Ring Mechanics

* **Fill Ring**: Populated by `csilk` with empty UMEM frame addresses to accept incoming packets from NIC DMA.
* **Rx Ring**: Written by NIC DMA when Ethernet/IP/TCP frames arrive; consumed directly by `csilk` without copying.
* **Tx Ring**: Written by `csilk` with response UMEM frame addresses for direct zero-copy NIC transmission.
* **Completion Ring**: Written by NIC DMA upon transmit completion; consumed by `csilk` to return frames to the free pool.

```c
csilk_ctx_t* csilk_ctx_from_xdp_frame(csilk_xdp_umem_pool_t* pool, uint64_t addr, uint32_t len);
```

---

## 3. io_uring Kernel SQPOLL & IOPOLL Zero-Syscall Integration

### 3.1 Kernel SQ Thread Polling (`uring_sqpoll.c`)

Configures `io_uring` parameters to spin a dedicated kernel SQ thread:

```c
struct io_uring_params params = {0};
params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_IOPOLL;
params.sq_thread_idle = 2000;              /* Timeout before kernel SQ thread sleeps */
params.sq_thread_cpu = target_cpu_core;    /* CPU core pinning */
```

* **Wakeup Trigger**: Checks `*sq_ring->flags & IORING_SQ_NEED_WAKEUP`. Executes `io_uring_enter()` with `IORING_ENTER_SQ_WAKEUP` only when kernel SQ thread has entered sleep.

### 3.2 Fixed File Descriptors (`IORING_REGISTER_FILES`)

Pre-registers Socket FDs and File FDs into the kernel `io_uring` instance. Operations pass `IOSQE_FIXED_FILE` indices to bypass VFS `fget()`/`fput()` atomic lock contention.

---

## 4. Zero-Copy Scatter-Gather Vector I/O Pipeline

### 4.1 String View / Slice Parser (`http1_zerocopy.c`)

Replaces heap allocation with zero-copy pointer views:

```c
typedef struct {
    const char *data;  /* Points directly to Rx UMEM or socket buffer offset */
    size_t      len;   /* String slice length */
} csilk_str_view_t;

typedef struct {
    csilk_str_view_t name;
    csilk_str_view_t value;
} csilk_header_slice_t;
```

### 4.2 Scatter-Gather Vector Submission (`uring_vector.c`)

HTTP Status Line, Headers, and Body are sent via `io_uring_prep_writev2()` without string concatenation:

```c
struct iovec iov[3];
iov[0].iov_base = (void*)status_line_str;  /* Static flash storage */
iov[0].iov_len  = status_line_len;
iov[1].iov_base = (void*)headers_raw_buf;  /* Context Arena allocation */
iov[1].iov_len  = headers_len;
iov[2].iov_base = (void*)body_ptr;        /* Raw payload memory */
iov[2].iov_len  = body_len;

io_uring_prep_writev2(sqe, fd, iov, 3, 0, 0);
```

---

## 5. Public API Contracts & Capability Probing

### 5.1 `include/csilk/core/io_perf.h`

```c
#ifndef CSILK_IO_PERF_H
#define CSILK_IO_PERF_H

#include <stddef.h>
#include <stdint.h>
#include "csilk/csilk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CSILK_IO_MODE_STANDARD = 0,    /* Standard libuv / socket event loop */
    CSILK_IO_MODE_URING    = 1,    /* Basic io_uring polling */
    CSILK_IO_MODE_SQPOLL   = 2,    /* io_uring kernel SQPOLL zero-syscall mode */
    CSILK_IO_MODE_XDP      = 3     /* AF_XDP hardware zero-copy UMEM bypass */
} csilk_io_mode_t;

typedef struct {
    csilk_io_mode_t active_mode;        /* Currently active I/O mode */
    int             has_xdp_zerocopy;   /* True if NIC driver supports zero-copy */
    int             has_sqpoll;         /* True if kernel SQPOLL is enabled */
    int             has_iopoll;         /* True if IOPOLL is supported */
    char            nic_name[32];       /* Bound physical NIC interface name */
} csilk_io_perf_info_t;

/**
 * @brief Probes Linux kernel capabilities and hardware NIC features.
 * @return Probe result structure.
 */
csilk_io_perf_info_t csilk_io_perf_probe(void);

/**
 * @brief Enables AF_XDP zero-copy hardware bypass engine.
 * @param app Application handle.
 * @param ifname Physical NIC interface name (e.g., "eth0").
 * @param queue_id NIC hardware queue ID.
 * @return 0 on success, negative value on failure and fallback.
 */
int csilk_io_perf_enable_xdp(csilk_app_t* app, const char* ifname, uint32_t queue_id);

/**
 * @brief Enables io_uring kernel SQPOLL zero-syscall polling.
 * @param server Server handle.
 * @param cpu_core Target CPU core ID for SQ thread pinning.
 * @return 0 on success, negative value on failure and fallback.
 */
int csilk_io_perf_enable_sqpoll(csilk_server_t* server, int cpu_core);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_IO_PERF_H */
```

---

## 6. Verification & Test Plan

1. **`test_af_xdp_zerocopy.c`**: Unit test UMEM pool creation, hugepage alignment, and XSK Fill/Rx/Tx/Completion ring index updates.
2. **`test_uring_sqpoll.c`**: Test socket read/write operations under `IORING_SETUP_SQPOLL` mode, verifying `IORING_SQ_NEED_WAKEUP` kernel thread wakeup logic.
3. **`test_http1_zerocopy.c`**: Unit test `csilk_str_view_t` slice header parsing and `writev` scatter-gather vector response formatting.
4. **`test_io_perf_fallback.c`**: Test capability probing and graceful fallback when executed in restricted container environments lacking `CAP_SYS_ADMIN` privileges.
