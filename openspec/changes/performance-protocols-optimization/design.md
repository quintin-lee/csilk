## Context

The current `csilk` architecture is optimized for HTTP/1.1 and HTTP/2 over TCP. It uses a Radix Tree for routing and an Arena allocator for memory management. While efficient, the routing engine uses scalar string comparisons, and the networking stack lacks HTTP/3 (QUIC) support, which is increasingly important for modern, low-latency web applications. Furthermore, the Arena allocator does not explicitly account for cache-line alignment, which can lead to performance degradation due to false sharing in high-concurrency multi-worker scenarios.

## Goals / Non-Goals

**Goals:**
- Implement HTTP/3 support via a pluggable driver or direct integration with a QUIC library.
- Accelerate Radix Tree routing using SIMD (AVX2 for x86_64, NEON for ARM64).
- Finalize and optimize HTTP/2 Server Push and flow control.
- Ensure the Arena allocator is cache-line aligned (64 bytes).
- Maintain backward compatibility for existing HTTP/1.1 and HTTP/2 TCP clients.

**Non-Goals:**
- Rewriting the entire core networking logic (keep libuv as the primary event loop).
- Implementing full HTTP/3 multiplexing and congestion control from scratch (use a library).
- Supporting legacy architectures without SIMD support (provide scalar fallback).

## Decisions

### 1. QUIC Library Selection: ngtcp2 vs. quiche
- **Decision**: Use `ngtcp2`.
- **Rationale**: `ngtcp2` is written in C (consistent with the rest of the codebase) and is highly modular, allowing it to use `gnutls` or `openssl` (already a dependency) for TLS. It provides the protocol implementation without forcing a specific I/O model, making it easier to integrate with libuv.
- **Alternatives**: `quiche` (Rust-based) was considered but adds a complex cross-language build dependency.

### 2. SIMD Routing Implementation
- **Decision**: Use architecture-specific intrinsics (AVX2/NEON) with a runtime detection fallback to scalar matching.
- **Rationale**: Path matching is a bottleneck in deep routing trees. Vectorizing the segment comparison can process 16-32 bytes in a single instruction cycle.
- **Alternatives**: Using a third-party SIMD library like `highway` was considered but would increase binary size and dependency complexity.

### 3. Arena Alignment (64-byte)
- **Decision**: Enforce 64-byte alignment for the start of every Arena chunk and every allocation within it.
- **Rationale**: 64 bytes is the standard cache-line size for most modern CPUs (Intel, AMD, ARM). This prevents "false sharing" where two worker threads on different cores update memory in the same cache line, causing constant invalidation and synchronization overhead.

### 4. HTTP/2 Server Push Integration
- **Decision**: Expose a `csilk_push_promise` API that can be called within a handler before the main response is sent.
- **Rationale**: This follows the `nghttp2` model and provides the most flexibility for application developers to push resources (CSS, JS) preemptively.

## Risks / Trade-offs

- **[Risk]**: Increased memory overhead from 64-byte alignment. → **Mitigation**: Small allocations will still be packed within the Arena; the alignment only applies to the *offset* within the chunk.
- **[Risk]**: Complexity of managing UDP and TCP concurrently. → **Mitigation**: Use separate libuv handles (`uv_udp_t` and `uv_tcp_t`) and unify them in the `csilk_server_s` structure.
- **[Risk]**: Portability issues with SIMD intrinsics. → **Mitigation**: Strict use of `#ifdef` for ARCH detection and providing a robust scalar fallback path.
