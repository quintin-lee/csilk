# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Security
- **Sensitive buffer zeroing**: Zero sensitive buffers after use in csrf, jwt, session, and websocket modules to prevent data leakage.
- **JWT integer overflow guards**: Add overflow protection to base64 length calculations in JWT parsing.

### Fixed
- **clang-tidy warnings**: Resolve static analyzer warnings in source and test files including array bounds, memory leaks, and null pointer dereferences.
- **Route macro safety**: Wrap route macros in `do { } while(0)` for safe use in control flow statements.
- **CI compatibility**: Bump upload/download-artifact to v6 for Node 24 support, skip FlameGraph upload when no samples collected, fix benchmark-results upload path.
- **macOS compatibility**: Add portable `explicit_bzero` shim for macOS builds.

### Changed
- **Include guards modernized**: Replace `#ifndef`/`#define` include guards with `#pragma once` across all 38 public headers.
- **API documentation**: Add Doxygen documentation to undocumented public API functions in middleware, server, and group headers.

## [0.3.0] - 2026-06-27

### Added
- **io_uring backend (Linux-only, optional)**: Full event loop, accept, read, write, and timer implementation using `CSILK_USE_URING=ON` at build time. Square-submission-polling (SQPOLL) with automatic fallback. Per-worker thread pools with lock-free dispatch queue. All 122 tests pass.
- **Documentation**: Updated all docs (architecture, build guide, test guide, deployment, performance tuning, troubleshooting, design) with comprehensive io_uring backend coverage.
- **Zero-copy HTTP Parsing** — Integrated C23-style string views (`csilk_str_view_t`) for HTTP headers, URLs, and bodies, referencing network receive buffers directly to eliminate heap malloc/free churn.
- **Deep struct freeing** — Added `csilk_struct_free_reflect` to recursively free nested struct pointers inside the reflection engine.
- **Static cyclic reference detection** — Added compile/startup-time DFS graph cycle-finding algorithm to validate registered reflection types and prevent recursion stack overflows.
- **Fuzz testing in CI**: Re-enabled fuzz testing job (clang-19 expected available on Ubuntu 24.04 by June 2026).
- **Extended test coverage**: WAF (4→9), Session (5→8), Recovery (1→4), CSRF (3→7), Workflow Lifecycle (1→3).
- **Zero-copy chunked write**: `_csilk_send_data_owned()` eliminates double-allocation/copy in chunked transfer encoding path.
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
- **Atomic builtins standardization** — Replaced all legacy compiler-dependent GCC `__sync_*` atomics with standard C11 `<stdatomic.h>` APIs.
- **Multi-worker loop safety** — Removed hardcoded `uv_default_loop()` reference, dynamically resolving the active worker thread's event loop to prevent multi-worker data races.
- **HTTPS read path optimization** — SSL read buffers are now allocated from the connection arena instead of the stack, keeping decrypted data safe for zero-copy string views.
- **Arena safety**: Added overflow guards and zero-size sentinel handling in `csilk_arena_alloc`.
- **Middleware middleware**: Added WAF (Web Application Firewall) to 15 built-in middleware.
- **Admin storage limit test**: Fixed `test_admin` storage overflow to store non-null values.
- **`_csilk_trigger_hooks`**: Made non-static and declared in `server_internal.h`
  so the H2 module can fire lifecycle hooks.
- **`pool_put`**: Now calls `csilk_h2_free_streams` to clean up any H2 stream
  contexts before returning the client to the free pool.
- **Version bumped**: 0.2.5 → 0.3.0 across all 18 version references.
- **Constants migrated**: `CSILK_DEFAULT_*` (5), `CSILK_MAX_PARAMS`,
  `CSILK_MAX_STORAGE`, `CSILK_MAX_CHILDREN`, `MAX_REG_STRUCTS`, `MAX_IP_ENTRIES`,
  `WINDOW_SIZE` converted to `static constexpr` and moved to appropriate headers.
- **Connection Pool**: Pool size of 32 clients; pool drained in `csilk_server_free`.
- **Streaming Response**: `csilk_response_write/end` now sets `is_async` flag to prevent double-write; chunked headers respect client `Connection: close` header.
- **Static Middleware**: Added `Accept-Ranges: bytes` header on all static responses.
- **Streaming Cleanup**: Terminal chunk write callback closes connection instead of leaving cleanup to timer (fixes use-after-free).
- **Header Location**: `context_internal.h` moved from `src/core/` to `include/`; `src/core` include path removed from CMakeLists.txt.
- **Doxygen Documentation**: Completed full Doxygen comments across all 37 source/header files with `@brief`, `@param`, `@return`, `@note` annotations.

### Fixed
- **io_uring SQE starvation**: `csilk_client_write` could silently drop responses when the io_uring Submission Queue ring was full. Added retry loop with backoff.
- **Stale keep_alive in on_write_done**: llhttp 9.3.1 clears `F_CONNECTION_CLOSE` after `on_message_complete` returns, causing `llhttp_should_keep_alive()` in write-completion callbacks to return the wrong value. Cached the decision in `client->keep_alive` when it's computed in `_csilk_send_response`.
- **Zero-copy form body parsing**: Fixed `csilk_parse_form_urlencoded` to use explicit body length (`csilk_arena_strndup` instead of `csilk_arena_strdup`) when zero-copy HTTP body references llhttp's TCP buffer which is not null-terminated at the body boundary.
- **ASan leaks**: Resolved memory leaks in new tests and Doxyfile generation.
- **macOS compatibility**: `fdatasync` → `fsync`, `SOCK_NONBLOCK` handling.
- **CI ASan suppression**: Added suppression for macOS false positives.
- **Arena TLS free list leak**: Added `csilk_arena_flush_free_list()` called on server free to prevent ASAN-detected leaks when server runs on a non-main thread.
- **MQ realloc overflow**: Added integer overflow guards and NULL checks in monitor array, global middleware array, and per-topic handler array growth paths.
- **SQL injection in `csilk_db_query_param_json`**: Added standard SQL single-quote doubling escaping.
- **HTTP parser memory leaks**: `on_url` max URL exceeded, `on_header_value` max size exceeded / buffer grow failure now free `current_url`, `current_header_field`, and `current_header_value`.
- **app.c server leak on error**: `csilk_server_new(NULL)` succeeds when `csilk_router_new()` fails; added `csilk_server_free()` in failure path.
- **hot_reload.c resource leak**: `dlclose`/`FreeLibrary` not called when `dlsym`/`GetProcAddress` or init function fails.
- **WAF null context segfault**: `csilk_waf_middleware(nullptr)` crashed on unblocked path calling `csilk_next(nullptr)`.
- **4 const-qualifier warnings**: `bounded_buf.c` return type and `static.c` C23 `strchr` overload.
- **GCC builtin atomics**: `perm.c` `__sync_val_compare_and_swap` → C11 `atomic_compare_exchange_strong`.
- Fixed `csilk_parse_form_urlencoded` Content-Type check logic (strict `application/x-www-form-urlencoded` check).
- Fixed memory leak in static middleware: `body_is_managed = 1` for full file buffer ensures cleanup.
- Fixed `csilk_ctx_cleanup` + timer interaction in streaming response lifecycle.
- Fixed 3 `csilK_` typos in server.c (pool_get/pool_put parameter types) and session.c (typedef).

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
