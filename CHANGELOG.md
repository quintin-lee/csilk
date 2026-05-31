# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **ABI opaque type conversion**: Moved internal struct definitions (`csilk_ctx_s`, `csilk_server_s`) from `include/csilk/core/` to `src/core/`. All non-framework code now accesses context state exclusively through the public accessor API.
- **Deferred cleanup API** (`csilk_ctx_defer` / `csilk_ctx_defer_free`): Panic-safe resource management across `setjmp`/`longjmp` boundaries. Protects heap allocations, file descriptors, and mutex locks from leaking during panic recovery.
- **SIMD-accelerated router**: AVX2 path matching on x86_64 and ARM NEON on aarch64. CMake auto-detection with `-mavx2` flag.
- **Lock-free per-worker connection pool**: Replaced mutex-based pool with per-worker lock-free pool for multi-core scaling.
- **macOS 14 ARM64 CI support**: Re-enabled macOS in CI matrix with `fdatasync`→`fsync` and `SOCK_NONBLOCK` fallback.
- **Real-time CPU flame graph**: Backtrace sampling and flame graph rendering in admin dashboard.
- **TypeScript/Python SDK generation**: Auto-generate API clients from OpenAPI spec.
- **Dynamic AI tool discovery**: MCP-like tool discovery API for agentic workflows.
- **Constant-time JWT signature comparison**: Replaced `strcmp` with constant-time comparison.
- **Python scaffold tool**: Rewrote `csilkskel` from C to interactive Python tool.
- **Hot reload support**: `csilk_server_set_router` for runtime router replacement.

### Changed
- **Arena safety**: Added overflow guards and zero-size sentinel handling in `csilk_arena_alloc`.
- **Middleware middleware**: Added WAF (Web Application Firewall) to 15 built-in middleware.
- **Admin storage limit test**: Fixed `test_admin` storage overflow to store non-null values.

### Fixed
- **ASan leaks**: Resolved memory leaks in new tests and Doxyfile generation.
- **macOS compatibility**: `fdatasync` → `fsync`, `SOCK_NONBLOCK` handling.
- **CI ASan suppression**: Added suppression for macOS false positives.

## [0.3.0] - 2026-05-30

### Added
- **HTTP/2 Phase 1 — Session scaffolding**: TLS ALPN negotiation (`h2` vs
  `http/1.1`), nghttp2 session initialisation, `csilk_h2.h` public API with
  `csilk_h2_init_session`, `csilk_h2_process_data`, `csilk_h2_get_or_create_stream`,
  `csilk_h2_free_streams`, and `send_callback` for frame serialisation.
- **HTTP/2 Phase 2 — Request dispatch and response**: Extracted
  `_csilk_dispatch_request` for unified routing across HTTP/1.1 and HTTP/2.
  Implemented nghttp2 callbacks (`on_header_callback` for pseudo-header + regular
  header parsing, `on_frame_recv_callback` for dispatch on END_STREAM,
  `on_data_chunk_recv_callback` for body accumulation, `on_stream_close_callback`
  for context cleanup). Added `csilk_h2_send_response` with
  `body_read_callback` data provider for streaming response bodies.
- **`test_h2` test suite**: Registered in `cmake/tests.cmake`.
- **C23 language standard**: Upgraded from C11 to C23 (`CMAKE_C_STANDARD 23`).
  Converted `#define` constants to `static constexpr` for type-safe compile-time
  values. Removed 6 `#include <stdbool.h>` lines (now a C23 keyword).

### Changed
- **`_csilk_trigger_hooks`**: Made non-static and declared in `server_internal.h`
  so the H2 module can fire lifecycle hooks.
- **`pool_put`**: Now calls `csilk_h2_free_streams` to clean up any H2 stream
  contexts before returning the client to the free pool.
- **Version bumped**: 0.2.5 → 0.3.0 across all 18 version references.
- **Constants migrated**: `CSILK_DEFAULT_*` (5), `CSILK_MAX_PARAMS`,
  `CSILK_MAX_STORAGE`, `CSILK_MAX_CHILDREN`, `MAX_REG_STRUCTS`, `MAX_IP_ENTRIES`,
  `WINDOW_SIZE` converted to `static constexpr` and moved to appropriate headers.

### Fixed
- **Client pool data race in multi-worker mode**: `pool_get`/`pool_put` accessed
  `client_pool` and `client_pool_count` without synchronization. In multi-worker
  mode, `on_new_connection` runs on any event loop thread, causing two threads
  to acquire the same client object. This triggered a libuv assertion crash:
  `uv_accept: Assertion 'server->loop == client->loop' failed`.
  Added `pool_mutex` to protect all pool operations.

## [0.2.5] - 2026-05-29

### Fixed
- **Client pool data race in multi-worker mode**: `pool_get`/`pool_put` accessed
  `client_pool` and `client_pool_count` without synchronization. In multi-worker
  mode, `on_new_connection` runs on any event loop thread, causing two threads
  to acquire the same client object. This triggered a libuv assertion crash:
  `uv_accept: Assertion 'server->loop == client->loop' failed`.
  Added `pool_mutex` to protect all pool operations.

## [0.2.4] - 2026-05-28

### Added
- **Redis Database Driver**: New `src/drivers/redis.c` driver using hiredis.
  Supports connection pooling with password auth and DB index selection.
  Maps Redis reply types to tabular results: GET→1-row, HGETALL→field/value
  pairs, KEYS/LRANGE→N-row flat arrays. Transactions via MULTI/EXEC/DISCARD.

## [0.2.3] - 2026-05-28

### Added
- **Unified Admin Dashboard**: Web-based real-time monitoring dashboard at `/admin` with
  HTTP metrics, workflow execution graphs, MQ queue status, DB pool telemetry, AI model
  call tracking, and process metrics. Serves `admin_ui.html` SPA with WebSocket live events.
- **MongoDB Database Driver**: New `src/drivers/mongodb.c` driver using libmongoc.
  Supports connection pooling and unified DB query interface.
- **MQ Message Status Monitoring**: Real-time MQ events, depth tracking, and JSON stats
  endpoint for admin dashboard integration.
- **Global AI Telemetry**: `src/ai/ai.c` now tracks model calls, token counts, and latency
  for admin dashboard consumption.
- **Global DB Telemetry**: `src/data/db.c` tracks pool size, active connections, and
  query latency across all database drivers.

### Fixed
- **test_workflow_monitor SEGFAULT**: Fixed heap-buffer-overflow caused by
  `calloc(1, 1024)` — `csilk_ctx_t` is 2944 bytes, allocated buffer was too
  small. Under ASan this triggered a SEGFAULT on every CI run.
- **scaffold `csilk_perm_auto_middleware_passthrough`**: Replaced with
  existing `csilk_perm_auto_middleware` — the former never existed,
  causing compile failure in core API + perm mode.
- **MQ recovery regression**: Fixed message queue recovery after connection drop.
- **Admin struct privacy**: Resolved incomplete type for `csilk_ctx_t` in admin module.
- **Mermaid syntax**: Fixed workflow Mermaid visualization for version 10+ quoting.
- **test_timeout flakiness**: Fixed port conflict by adding server readiness sync.

### Changed
- **Header relocation**: `workflow_wal.h` moved from `src/app/` to
  `include/csilk/app/` to keep all headers under `include/`.
- **Admin scaffold**: `csilkskel` now includes admin dashboard setup by default.
- **Version bumped**: 0.2.1 → 0.2.3

## [0.2.2] - 2026-05-27

### Added
- **Symmetric/Asymmetric Cipher Driver**: New `csilk_cipher_driver_t` interface
  with AES-256-GCM encrypt/decrypt, RSA-2048 key generation, RSA-OAEP
  encrypt/decrypt, and RSA-PSS sign/verify. Includes a default OpenSSL EVP
  implementation (`src/crypto/cipher.c`). Pluggable via
  `csilk_server_set_cipher_driver()` — pass NULL to restore defaults.
- **Cipher Tests**: 8 test cases covering symmetric roundtrip, wrong tag
  rejection, bad key rejection, asymmetric roundtrip, sign/verify, custom
  driver plugin, custom keygen, and NULL context fallback.

### Changed
- **`csilk_ctx_t`**: Added `cipher_driver` field for per-request cipher access.
- **Project structure**: Added `src/crypto/` and `include/csilk/drivers/cipher.h`.

## [0.2.1] - 2026-05-25

### Added
- **Form URL-encoded Parser**: Added `csilk_parse_form_urlencoded()` and `csilk_get_form_field()` for `application/x-www-form-urlencoded` body parsing (P5-1).
- **Session Support**: Cookie-based in-memory session management with `csilk_session_init/start/set/get/destroy` API (P5-2).
- **HTTP Range Requests**: Static file middleware now supports `Range` header with 206 Partial Content responses (P5-3).
- **Request Validation Middleware**: `csilk_validate()` with REQUIRED/INT/STRING/EMAIL flags and min/max range validation (P5-4).
- **Connection Object Pool**: Reuses `csilk_client_t` objects via free list to reduce allocation overhead (P3-5).
- **URL Decoding**: Implemented `csilk_url_decode()` for percent-decoding query parameters.
- **SHA1/Base64 Known-Answer Tests**: 14 test cases covering RFC 3174 and RFC 4648 vectors.
- **WebSocket Integration Test**: Verified 101 Switching Protocols + `Sec-WebSocket-Accept` header.
- **Streaming Response Integration Test**: Verified chunked encoding with `csilk_response_write/end`.
- **Redirect Tests**: Enhanced with `csilk_redirect_simple`, 301/302/307 status codes, null-safety edge cases.

### Changed
- **Connection Pool**: Pool size of 32 clients; pool drained in `csilk_server_free`.
- **Streaming Response**: `csilk_response_write/end` now sets `is_async` flag to prevent double-write; chunked headers respect client `Connection: close` header.
- **Static Middleware**: Added `Accept-Ranges: bytes` header on all static responses.
- **Streaming Cleanup**: Terminal chunk write callback closes connection instead of leaving cleanup to timer (fixes use-after-free).
- **Header Location**: `context_internal.h` moved from `src/core/` to `include/`; `src/core` include path removed from CMakeLists.txt.
- **Doxygen Documentation**: Completed full Doxygen comments across all 37 source/header files with `@brief`, `@param`, `@return`, `@note` annotations.

### Fixed
- Fixed `csilk_parse_form_urlencoded` Content-Type check logic (strict `application/x-www-form-urlencoded` check).
- Fixed memory leak in static middleware: `body_is_managed = 1` for full file buffer ensures cleanup.
- Fixed `csilk_ctx_cleanup` + timer interaction in streaming response lifecycle.
- Fixed 3 `csilK_` typos in server.c (pool_get/pool_put parameter types) and session.c (typedef).

## [0.2.0] - 2026-05-23

## [0.1.0] - 2026-05-15

### Added
- Initial release with core routing, middleware, and server implementation.
- Support for JSON (cJSON), WebSocket, and YAML configuration.
- Built-in middleware: Logger, Recovery, Auth, CORS, CSRF, Rate Limiting, Static Files.
- Comprehensive Doxygen documentation.
