## 1. Environment & Dependencies

- [x] 1.1 Add `ngtcp2` and `nghttp3` to `CMakeLists.txt` via FetchContent.
- [x] 1.2 Update `config.h` and `config.c` to support HTTP/3 port and SIMD settings.

## 2. Arena Memory Optimization

- [x] 2.1 Update `src/core/arena.c` to enforce 64-byte alignment for all allocations.
- [x] 2.2 Add unit tests in `tests/test_arena.c` to verify 64-byte alignment.

## 3. SIMD Routing Acceleration

- [x] 3.1 Implement AVX2 segment matching in `src/core/router.c`.
- [x] 3.2 Implement NEON segment matching in `src/core/router.c`.
- [x] 3.3 Add runtime CPU feature detection for SIMD dispatching.
- [x] 3.4 Verify routing correctness with new SIMD paths in `tests/test_router.c`.

## 4. HTTP/2 Server Push

- [x] 4.1 Implement `csilk_push_promise` in `src/core/h2.c`.
- [x] 4.2 Add `csilk_push_promise` declaration to `include/csilk/response.h`.
- [x] 4.3 Add integration test for Server Push in `tests/test_h2.c`.

## 5. HTTP/3 & QUIC Implementation

- [x] 5.1 Implement UDP listener and packet handling in `src/core/connection.c` using libuv `uv_udp_t`.
- [x] 5.2 Implement HTTP/3 request/response lifecycle in new `src/core/h3.c`.
- [x] 5.3 Integrate HTTP/3 with the unified `_csilk_dispatch_request` path.
- [x] 5.4 Add integration tests for HTTP/3 in new `tests/test_h3.c`.

## 6. Verification & Benchmarking

- [x] 6.1 Run `wrk` benchmarks to measure performance gains from SIMD routing.
- [x] 6.2 Run `h2spec` and `quic-interop-runner` to verify protocol compliance.
- [x] 6.3 Finalize optimization report and update `BENCHMARK_REPORT.md`.
