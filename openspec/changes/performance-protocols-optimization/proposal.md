## Why

The `csilk` framework is a high-performance C web framework currently moving towards v1.0. To maintain its competitive edge and support modern web standards, it needs to transition from a pure HTTP/1.1 and early HTTP/2 architecture to one that supports HTTP/3 (QUIC) and leverages hardware acceleration for routing. These changes will significantly reduce latency, improve throughput under high concurrency, and provide better support for mobile and unreliable network environments.

## What Changes

- **HTTP/3 Support**: Integration of a QUIC library to support HTTP/3 over UDP.
- **SIMD Routing Acceleration**: Implementation of AVX2 and NEON vector instructions in the Radix Tree router.
- **HTTP/2 Enhancements**: Full implementation of Server Push (`csilk_push_promise`) and optimized flow control.
- **Arena Memory Optimization**: Cache-line alignment for Arena allocations to prevent false sharing in multi-worker environments.
- **Static File Zero-Copy Expansion**: Extending `sendfile` optimization to more response scenarios.

## Capabilities

### New Capabilities
- `http3-quic`: Support for the HTTP/3 protocol over UDP using a QUIC library.
- `simd-router`: SIMD-accelerated path matching for the Radix Tree router.
- `h2-server-push`: Support for HTTP/2 Server Push promises.
- `cache-aligned-arena`: Optimized Arena allocator for multi-core cache locality.

### Modified Capabilities
(None - initial capabilities for performance track)

## Impact

- **Networking Stack**: New `h3.c` and significant updates to `server.c`, `connection.c`, and `h2.c`.
- **Router Engine**: `router.c` will now include architecture-specific SIMD branches.
- **Memory Management**: `arena.c` will enforce 64-byte alignment for all allocations.
- **Dependencies**: New dependency on a QUIC library (e.g., `ngtcp2` or `quiche`).
- **Configuration**: New configuration options for HTTP/3 ports and SIMD enabling/disabling.
