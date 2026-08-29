# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **AsyncOp & Stream Lifetime Model**: Introduced reference-counted asynchronous operation lifecycle (`csilk_async_op_t`) with generation tag and request sequence validation against ABA, and asynchronous timer cleanup on event loop to prevent timer Use-After-Free.
- **HTTP/2 Trailer Support & Exactly-Once Dispatch**: Hardened nghttp2 callback pipeline to strictly distinguish initial request `HEADERS` from trailing `HEADERS` (Trailers), enforce exactly-once application dispatch, handle `RST_STREAM` before dispatch cleanly, and separate passive frame lookups (`csilk_h2_get_stream`) from active stream allocation (`csilk_h2_get_or_create_stream`).
- **HTTP/2 Zero-Copy Header Materialization**: Implemented `map_set_view` allowing zero-copy string views directly into nghttp2 memory buffers during request header ingestion, and single-pass header counting and `nghttp2_nv` response encoding.
- **HTTP/2 Stream Pool Deterministic Reset**: Formalized stream recycling contract with zero-syscall reuse, chunk arena preservation across stream lifecycles, and deterministic struct field resets.
- **Formalized Worker Ownership**: Worker threads own mutable connection and stream state; cross-worker unrefs dispatch back to owning worker event loop; lazy Message Queue initialization uses lock-free Double-Checked Locking (DCLP) with acquire/release memory barriers.
- **Modular CMake Target DAG & Package Export**: Established `csilk_base` as foundation, fully exported `yyjson`, bundled `llhttp`, and `nghttp2` targets, and normalized `pkg-config` `.pc` templates with `-D_GNU_SOURCE`.

### Performance & Memory
- **Per-Request Context Memory Reduction**: Compacted `csilk_ctx_t` layout with 16-bucket chained hash map and compact 4-slot embedded read buffers, reducing per-context memory footprint and improving 0-header throughput by +62.6% (11.35 M req/s).
- **HTTP/2 Stream Allocation Scaling**: High-performance per-connection adaptive hash table delivering 4.47 M stream-cycles/sec pool throughput, 26.2 ns lookup latency for 10 concurrent streams, and 62.8 ns lookup latency for 10,000 concurrent streams.

### Fixed
- **Connection UAF on Graceful Shutdown (CWE-416)**: In `close_active_clients()`, snapshot `client->next` before calling `csilk_io_close()` to prevent Use-After-Free when the close callback fires synchronously and modifies the active client linked list.
- **Data Race on Client State Read (CWE-362)**: Restructured `_csilk_client_check_recycle()` to only read `client->state` on the owning worker thread. Non-owner threads now dispatch recycle tasks exclusively, eliminating a TSAN-flagged data race on the non-atomic state field.
- **C23 `<stdbool.h>` Compliance**: Removed 4 unnecessary `#include <stdbool.h>` directives from `connection_state.c`, `mvcc_cache.c`, `json.h`, and `mvcc_cache.h`. C23 provides `bool`/`true`/`false` as built-in keywords.
- **Clang-Tidy SPA Fallback Memory**: Replaced heap allocation in `spa_fallback_handler` with arena-managed memory allocation, resolving static analyzer warnings.

## [0.5.3] - 2026-08-26

### Security Fixes

**Critical:**
- **Auth Pipeline Execution (CWE-285)**: Added missing `csilk_next(c)` when authentication succeeded in `csilk_auth_middleware`, preventing request pipeline starvation.
- **Session Key Zeroing (CWE-665)**: Corrected sensitive key zeroing timing to avoid clearing the session key prior to cache retrieval.
- **SQL Injection Defense (CWE-89)**: Added `csilk_db_exec_param()` with parameter escaping across all database drivers.
- **JWT Algorithm Confusion (CWE-327)**: Forced decoding and validation of the JWT header `alg` field against expected server algorithm, preventing `alg: "none"` and RSA/HMAC confusion attacks.
- **WebSocket Frame Integer Overflow (CWE-190, CWE-122)**: Added 64-bit frame payload length validation with 64 MB maximum threshold (`CSILK_WS_MAX_FRAME_PAYLOAD`) and integer overflow prevention.
- **AI Driver Double Free (CWE-415)**: Eliminated duplicate `csilk_json_free()` in `openai.c` error path on invalid URL schemes.

**High:**
- **Base64URL Table Initialization (CWE-20)**: Initialized all 256 entries in `b64_rev_table` to prevent invalid ASCII bytes from decoding as 0 (`'A'`).
- **Open Redirect Protection (CWE-601)**: Hardened `csilk_redirect` against scheme-relative `//` and case-insensitive pseudo-protocol bypasses (`javascript:`, `data:`, `vbscript:`).
- **CSRF Constant-Time Compare (CWE-208, CWE-352)**: Used `CRYPTO_memcmp()` for constant-time CSRF token comparison and set `http_only = 0` so frontend SPA can read the Double Submit cookie.
- **Multipart Binary Upload Safety (CWE-125)**: Replaced `strstr()` with binary-safe `_csilk_memmem()` to support uploads containing `\0` null bytes without boundary truncation.
- **HTTP Response Header CRLF Injection (CWE-113)**: Sanitized header keys and values in `map_set` and `map_add` by stripping `\r` and `\n` to prevent HTTP response splitting.
- **MQ WAL Frame Length Validation (CWE-190, CWE-122)**: Added strict bounds checking (`MQ_WAL_MAX_TOPIC_LEN` 64 KB, `MQ_WAL_MAX_PAYLOAD_LEN` 64 MB) during WAL recovery.

**Medium & Low:**
- **Cookie SameSite Attribute Support (CWE-352)**: Added `csilk_set_cookie_ex()` supporting `SameSite=Strict`, `Lax`, and `None`.
- **WAF Request Body Malicious Pattern Inspection (CWE-693)**: Added inspection of POST JSON and form request bodies for SQLi, XSS, and path traversal patterns.
- **MQ WAL Task Allocation OOM Guard (CWE-476)**: Added NULL checks on `strdup` and `malloc` in `_mq_append_wal()`.
- **Static File Range Header Memory Mutation (CWE-704)**: Removed in-place `*dash = '\0'` mutation of `Range` request header.
- **Static File Traversal Separator Robustness (CWE-22)**: Added support for Windows backslash `\` path separators in `contains_path_traversal()`.
- **gRPC Gateway Dynamic Payload Sizing (CWE-400)**: Replaced fixed 4KB stack buffer with arena-allocated dynamic buffers.
- **URL Percent Decoding Signedness (CWE-20)**: Added explicit `(unsigned char)` casts in `isxdigit()` within `csilk_url_decode()`.
- **Nonce Generation Bit-Shift Bound (CWE-330)**: Bound bit-shift widths to within 32 bits during pseudo-random fallback.
- **Blowfish Endianness Portability**: Explicitly extracted bytes via shift operations in Blowfish `fo()`.

### Added
- **OpenAI Streaming Driver Upgrade** (`src/drivers/ai/openai.c`): Full SSE streaming support (`stream = 1`) with real-time token dispatch (`on_chunk`), streamed `delta.tool_calls` dynamic reassembly across chunks (matching `index`, concatenating chunk arguments, support for pure tool calls and mixed modes), and stream token usage capture (`stream_options: {"include_usage": true}`).
- **Public Cipher API**: Added `csilk_symmetric_encrypt/decrypt` (AES-256-GCM), `csilk_rsa_generate_keypair`, `csilk_rsa_encrypt/decrypt`, `csilk_rsa_sign/verify` in `<csilk/core/cipher.h>` — standalone operations without request context.
- **Public HTTP/2 API** (`csilk/http/h2.h`): Promoted from internal `src/core/http/h2.h` to public `include/csilk/http/h2.h`; now includes `csilk_h2_init_session`, `csilk_h2_process_data`, `csilk_h2_get_or_create_stream`, `csilk_h2_free_streams`, `csilk_h2_remove_stream`, `csilk_h2_send_response`, `csilk_h2_submit_push`.
- **Public Flame Graph API** (`csilk/util/flamegraph.h`): Promoted from internal `src/util/flamegraph.h` to public `include/csilk/util/flamegraph.h`; now includes `csilk_flamegraph_start`, `csilk_flamegraph_stop`, `csilk_flamegraph_is_running`.

### Changed
- **Crypto module refactoring**: Split 711-line `src/crypto/crypto.c` into `crypto.c` (primitives: SHA-256, HMAC, UUID, RNG, nonce, ~297 lines) and `src/crypto/cipher_dispatch.c` (cipher dispatch: AES/RSA/JWT, ~350 lines). Moved `src/crypto/url.c` to `src/core/primitives/url.c` (HTTP parsing utility, not cryptography).
- **Include directory alignment**: All public headers now mirror the src/ module layout. Internal-only headers (`header_map.h`, `query.h`, `lfqueue.h`) remain in `src/` only.
- **Test count**: 211 → 213 (added cipher public API tests: 5 test functions).

### Fixed
- **clang-tidy**: Zero warnings on all changed files. to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.2] - 2026-08-23

### Fixed
- **JWT middleware NULL-key path**: `csilk_jwt_middleware(c, NULL)` no longer silently returns without sending a response. Now sends HTTP 500 `Internal Server Error` and aborts the handler chain, matching the behavior of other middlewares (ratelimit, csrf). Added `test_jwt_middleware_null_key` test case.
- **ASAN memory leaks**: Fixed heap leaks in `test_hot_reload_null` (missing `csilk_router_free`), `test_uring_fs` (use-after-free from premature `csilk_io_loop_close`), and `csilk_io_fs_sendfile` segfault on NULL request.
- **Clang build compatibility**: Added forward declarations for test functions defined after `main()` — GCC tolerates this but Clang rejects it.

### Added
- **SSE integration test** (`test_sse_integration`): 7 test cases covering SSE init, send, close, and header validation via live HTTP server.
- **MCP stdio test** (`test_mcp_stdio`): 5 test cases covering JSON-RPC initialize, tools/list, tools/call, unknown method, and missing params via forked child process.
- **Session integration test** (`test_session_integration`): 7 test cases for session start/get/set lifecycle.
- **Admin integration test** (`test_admin_integration`): 11 test cases covering `/admin/stats`, `/admin/`, `/admin/topology`, and 404 paths.
- **Middleware chain integration test** (`test_middleware_chain_integration`): 6 test cases verifying middleware execution order.
- **OpenAPI integration test** (`test_openapi_integration`): 5 test cases for OpenAPI JSON and Swagger UI endpoints.
- **ctx_json unit tests** (`test_ctx_json`): 10 test cases for `csilk_bind_json`, `csilk_bind_json_err`, `csilk_get_cookie`, `csilk_bind_reflect`.
- **JSON mutate edge-case tests**: Added null-idoc and null-mdoc guard cases to `test_json_mutate`.
- **Request ID readiness handler tests**: Added null-safety tests for `csilk_ready_check_handler`.

### Changed
- **Refactored server_lifecycle.c**: Extracted RCU management into `server_rcu.c` (569 lines) and driver injection into `server_driver.c` (59 lines).
- **Added .gcovr config**: Excludes non-testable files (`flamegraph.c`, `redis_storage.c`, `workflow_debug.c`, `uring_vector.c`) from coverage reports.

### Test Coverage
- **Total tests**: 213 (211 unit + 2 integration families)
- **Line coverage**: 66% (11,765/17,773 lines)
- **sse.c**: 22% → 77% (SSE integration test)
- **hot_reload.c**: 5% → 66% (dynamic library reload test)

## [0.5.1] - 2026-08-22

### Added
- **Formal Client Lifetime Verification & Owner Confinement**: Formally verified client lifecycle state machine across 100,000 reuse iterations, proving `client_destroy` executes strictly on the owning worker loop, non-owner threads enqueue generation-tagged recycle tasks (`_csilk_client_recycle_dispatch_cb`), and pending I/O / reference counters never underflow.
- **RCU / EBR Formal Verification & 512-Reader Scaling**: Added formal RCU lifecycle stress suite with 512 concurrent readers (static 256 + dynamic overflow slots) and 10,000 short-lived threads, proving zero dynamic slot leaks, safe TID reuse, and lock-free/wait-free read paths. Serialized router swaps under `config_mutex` for monotonic epoch progression.
- **HTTP/2 Stream Multiplexing Formal Lifecycle**: Formally verified stream multiplexing lifecycle (`stream_new`, `h2_stream_map` lookup/eviction with 16 inline buckets, `free_list` recycling, RST_STREAM, GOAWAY, and async completions) guaranteeing stream callbacks never access recycled contexts.
- **PMU-Guided Micro-Optimizations**: Micro-optimized critical paths (`client_ref`/`client_unref`, `pending_io_inc`/`pending_io_dec`, RCU nesting depth tracking, `g_dispatch_tls_registered` branch caching, and HeaderMap fast bitmask short-circuit), reducing core cycle footprint without compromising correctness.
- **Wait-Free MPSC Queue Hardening**: Added null-safety guards in `csilk_lfq_dequeue()` and automatic worker pool queue initialization, ensuring robust multi-producer dispatching across arbitrary thread topologies.
- **Ordered Teardown Sequence**: Enforced clean teardown order (`server_stop` -> drain active clients -> drain timers -> drain dispatch queues -> join workers -> stop hot reload -> EBR grace period -> destroy router -> close event loop -> free pools). Deferred MQ teardown past worker thread join to prevent async crashes on worker exit.

### Fixed
- **Dispatch Async Cleanup & Loop Draining**: Safely closed `wp->dispatch_async` and drained pending event loop handles during `csilk_server_free()`, preventing dangling handles in default libuv event loop.

## [0.5.0] - 2026-08-22

### Added
- **Hot-Reload Mutual Exclusion & Secure Temp Files**: Added `csilk_mutex_t reload_mutex` in `hot_reload_ctx_t` ensuring filesystem watcher debounce and manual `csilk_dev_hot_reload_trigger()` never race or concurrently mutate reload state. Enforced secure `mkstemp(0600)` with immediate failure reporting, and implemented complete OOM rollback (`dlclose`, `unlink`, `csilk_router_free`).
- **Adaptive io_uring Queue Sizing & Resource Fallback**: Replaced hardcoded 4096-entry ring initialization with adaptive fallback (1024 -> 512 -> 256 -> 128 -> 64) and proportional pool capacity, eliminating `-ENOMEM` errors on constrained container / VM environments (`RLIMIT_MEMLOCK`).
- **6-Tier Unified Memory Ownership Taxonomy**: Defined comprehensive 6-tier memory ownership model (`csilk_ownership_t`: `BORROWED`, `ARENA`, `OWNED`/`HEAP`, `TRANSFER`, `POOL`, `TLS_CACHE`) and stringifier `csilk_ownership_str()` in `<csilk/core/types.h>`. Standardized capacity-aware buffer cleanup and pool reclamation in `_csilk_ctx_cleanup()`, and unified response body memory replacement guards (`_csilk_free_response_body_if_needed()`).
- **3-Tier ABI Architecture & Strict Opaque Encapsulation**: Enforced strict `Public API → Opaque Handle → Internal Implementation` architecture across all 52 public headers under `include/`:
  - Converted `csilk_router_t` into a strictly opaque handle in `<csilk/core/router.h>`, moving `struct csilk_router_s` and trie node structures into `src/core/primitives/router_internal.h`.
  - Decoupled OpenSSL from public headers: `<csilk/core/hash.h>` now defines `csilk_sha1_ctx` and `csilk_sha256_ctx` as 64-bit aligned opaque memory buffers (128 bytes), verified at compile-time via `_Static_assert`.
  - Decoupled Backend I/O handles: removed `csilk/core/sys_io.h` from `<csilk/core/context.h>` and relocated `csilk_get_work_req` to `src/core/ctx/ctx_internal.h`.
- **Asynchronous Context Safety & Generation Tracking**: Added active reference counters (`_csilk_ctx_async_ref_incr` / `_csilk_ctx_async_ref_decr`), monotonically increasing request sequence numbers (`request_seq`), and UUID v4 request tags (`request_id`) to ensure async worker/DB/MQ callbacks cannot touch recycled contexts or cause use-after-free on keep-alive connections.
- **Connection Lifecycle State Machine**: Implemented explicit 9-state connection lifecycle (`csilk_conn_state_t`: `INIT`, `ACCEPTED`, `TLS`, `READING`, `PROCESSING`, `WRITING`, `STREAMING`, `CLOSING`, `CLOSED`) with invariant transition checks (`csilk_conn_set_state`, `csilk_conn_get_state`, `csilk_conn_state_str`), preventing use-after-free, double close, double free, and async write/streaming race conditions.
- **I/O & Sync Abstraction Layer**: Standardized unified, cross-backend `csilk_io_*`, `csilk_thread_*` (`csilk_thread_create`, `csilk_thread_join`, `csilk_thread_self`, `csilk_thread_setaffinity`), and `csilk_barrier_*` (`csilk_barrier_init`, `csilk_barrier_wait`, `csilk_barrier_destroy`) APIs in `<csilk/core/sys_io.h>` and `<csilk/core/sync.h>`.

- **Streaming Backpressure & Watermark Flow Control**: Implemented per-connection outbound queue backpressure across HTTP/1.1 chunked streaming (`csilk_response_write`), SSE (`csilk_sse_send`), and WebSocket (`csilk_ws_send`). Added configurable high water marks (`write_high_water_mark`, default 64KB), low water marks (`write_low_water_mark`, default 16KB), maximum buffer limits (`max_write_buffer_size`, default 16MB), and asynchronous drain callback registration (`csilk_on_drain` / `csilk_set_write_watermarks`).
- **Context Storage Destructor Support**: Added `csilk_set_ex()` with `csilk_destructor_t` callback for RAII cleanup of heap values when context arenas reset. JWT middleware now automatically binds `csilk_json_free` to `jwt_payload`.
- **Typed Zero-Copy Views**: Added `csilk_view_t` (`const char* data; size_t len;`) with explicit borrowed view accessors (`csilk_get_query_view`, `csilk_get_param_view`, `csilk_get_header_view`, `csilk_get_body_view`) distinguishing zero-copy parser buffers from owned NUL-terminated arena strings.
- **JWT Validation Policies & Options**: Added `csilk_jwt_flags_t` (`CSILK_JWT_REQUIRE_EXP`, `CSILK_JWT_REQUIRE_NBF`, `CSILK_JWT_REQUIRE_IAT`), `csilk_jwt_options_t` (algorithm, flags, clock skew leeway), `csilk_jwt_verify_options()`, and `csilk_jwt_middleware_options()` for strict JWT claim enforcement.
- **Arena Calloc & Multi-Tier TLS Caching**: Added `csilk_arena_calloc()` for zero-initialized arena allocations, 3-tier thread-local chunk free lists (4KB, 16KB, 64KB) with `max_total_bytes` constraint, and worker thread exit cleanup (`csilk_arena_flush_free_list`).

- **Crypto driver extensibility**: `csilk_crypto_driver_t` now supports `sha1` (20-byte digest) and `bcrypt_hash` (password hashing) callbacks with internal dispatch wrappers `_csilk_sha1()` and `_csilk_bcrypt_hash()` — drivers can replace the built-in software implementations.
- **`csilk_cond_broadcast()`**: New function in `<csilk/core/sync.h>` for broadcasting all waiters on a condition variable. Bridges the gap between `pthread_cond_broadcast` and libuv (which has no broadcast primitive).
- **Crypto module tests**: Comprehensive property-based tests for SHA-256, HMAC-SHA256, Base64/Base64URL roundtrip, `csilk_crypto_fill_random`, `csilk_crypto_generate_nonce`, and `csilk_url_decode` edge cases in `tests/crypto/test_crypto.c`.

### Changed
- **OpenSSL-Backed Cryptographic Primitives**: Replaced hand-rolled SHA-256 (`csilk_sha256_*`), HMAC-SHA256 (`csilk_hmac_sha256`), and SHA-1 (`csilk_sha1_*`) implementations with system OpenSSL primitives, and re-engineered `bcrypt` to use OpenSSL `RAND_bytes()` / `RAND_priv_bytes()`, constant-time `CRYPTO_memcmp()`, secure memory zeroization via `OPENSSL_cleanse()`, and re-entrant thread-safe cipher state.
- **Portable Release Binary Defaults**: Changed Release build default to portable binaries (without `-march=native`), preventing `SIGILL` crashes on older CPUs, Docker containers, and CI artifacts. Host-native CPU instruction set tuning is now explicitly opt-in via `-DCSILK_ENABLE_NATIVE_ARCH=ON` (enabled automatically in benchmark scripts and CI benchmark workflows).

- **Server Core Pure Abstraction**: Eliminated all direct `uv_*` references from `src/core/server/` (`connection.c`, `server_lifecycle.c`, `server_shutdown.c`, `server_worker.c`), replacing them with `csilk_io_*` and `csilk_thread_*`/`csilk_barrier_*`.


- **io_uring Architecture Streamlining**: Eliminated parallel duplicate server state machines (`uring_server.c`, `uring_connection.c`, `uring_event_loop.c`), consolidated the driver under `src/core/uring/uring_io.c`, and unified single-track server lifecycle execution across backends.
- **Router Prefix Trie Architecture & Rollback**: Aligned router documentation to reflect segment-based prefix trie architecture and fixed wildcard parameter backtracking on method mismatch or handler failure.
- **Handler Chain Boundary Safety**: Added explicit `handler_count` check in `csilk_next()` to guard against corrupted or unterminated handler arrays.
- **Thread Confinement & Dispatch**: Explicitly documented worker-local `active_clients` confinement; cross-thread work must use `csilk_dispatch()`.
- **Barrier lifecycle**: `uv_barrier_t` in `src/core/server/server_lifecycle.c` is now heap-allocated (`calloc`) to prevent use-after-free when worker threads outlive the stack-local barrier. `uv_barrier_init` return value is now checked.
- **Threading abstraction**: `src/core/uring/uring_thread_pool.c` replaced raw `pthread_mutex_t`/`pthread_cond_t` with `csilk_mutex_t`/`csilk_cond_t` from `<csilk/core/sync.h>` for cross-backend consistency.
- **Header hygiene**: Removed `messaging/mq_internal.h` transitively included via `include/csilk/core/internal.h`. Files needing `_csilk_mq_new`/`_csilk_mq_free` now include `messaging/mq_internal.h` explicitly.
- **Code cleanup**: Standardized `nullptr` → `NULL` across 1200+ occurrences for C23 consistency. Fixed `-Wcomment` (connection.c), `-Wformat` (qdrant.c, workflow_dsl.c), and `-Wformat` (session.c strdup null check).

### Fixed
- **Owned Event Loop Cleanup in csilk_server_free**: Added `csilk_io_loop_close` and memory release for `server->loop` when `server->loop_owned` is true, eliminating io_uring file descriptor leaks across rapid server instantiations.
- **Multi-Worker Sendfile & Hook Synchronization**: Synchronized multi-worker sendfile operations using `CSILK_HOOK_SERVER_START` and added timeout / retry policies preventing deadlocks during multi-worker socket binding.
- **io_uring Backend Cancellation & Listen Socket Safety**: Fixed `csilk_io_timer_stop()` using targeted `io_uring_prep_cancel64` with pointer and generation tagging instead of broad cancellation, preventing timer stops from inadvertently cancelling server listening socket SQEs.
- **io_uring Async & Signal Poll Notification Reliability**: Added `read() > 0` validation for `URING_OP_POLL_ASYNC` and `URING_OP_POLL_SIGNAL` before firing callbacks to prevent spurious executions.
- **Dual-Backend Build & Field Isolation**: Isolated io_uring-specific handle fields (`generation`, `fd`) in `src/core/server/connection.c` under `#ifdef CSILK_USE_URING`, and introduced portable `reject_connection()` helper ensuring clean compilation and 100% test pass rate across both libuv and io_uring backends.
- **Router Match Debug Logging Safety**: Added defensive null check for `mh` in `csilk_router_match_ctx()` log statement, eliminating `clang-analyzer-core.NullDereference` during `clang-tidy` checks.
- **Multi-Worker Startup Barrier Deadlock**: Eliminated infinite hangs and deadlocks during multi-worker initialization when worker allocations (`worker_data_t`) or thread creations (`csilk_thread_create`) fail midway, compensating the barrier count for unspawned workers and performing clean graceful aborts.
- **Dynamic TCP Read Buffers**: Expanded `read_buffers` dynamically (doubling initial 16 slots) to prevent data dropping when a request requires >16 TCP reads.
- **Atomic Max Connections**: Converted `max_connections` check to atomic CAS reservation (`_csilk_server_try_acquire_connection`) and rollback to eliminate high-concurrency TOCTOU race conditions.
- **JWT Memory Leak**: Bound automatic destructor via `csilk_set_ex()` to free cJSON payload heap allocations on context reset.
- **uv_barrier_t UAF**: Fixed use-after-free in multi-worker server startup where a stack-allocated `uv_barrier_t` was destroyed by the main thread while worker threads still held its address. Barrier is now heap-allocated and freed after all workers join.
- **internal.h MQ leak**: Removed `#include "messaging/mq_internal.h"` from `include/csilk/core/internal.h` to stop transitively exposing MQ internals (e.g., `csilk_mq_t`) to every file including the umbrella header.



## [0.4.0] - 2026-08-13

### Changed
- **Directory structure reorganization**: Moved `base64.c`, `sha1.c`, `url.c`, `uuid.c`, `crypto.c` (formerly `utils.c`) from `src/core/server/` into new `src/crypto/` module; moved `bcrypt.c` and `blowfish_sboxes.h` into `src/crypto/` (merged `src/security/`); moved `admin.c` from `src/core/config/` to `src/app/`; reorganized tests from `tests/data/` into `tests/security/` and `tests/drivers/db/`; removed redundant `include/csilk/core/admin.h` re-export wrapper; removed `CSILK_DATA_SOURCES` CMake variable (inlined into `CSILK_DRIVER_SOURCES`).

### Added
- **Embedded SIMD Vector Index Engine**: 32-byte aligned AVX2 SIMD distance kernels (`csilk_simd_vector_cosine`, `csilk_simd_vector_l2`, `csilk_simd_vector_dot`) and multi-layer HNSW skip-graph index engine (`csilk_hnsw_index_t`) supporting $O(\log N)$ ANN vector similarity search (`csilk_vector_db_new_embedded`).
- **eBPF XDP Dynamic WAF & OTLP APM Dashboard**: BPF-Map hot-reloading dynamic WAF rule engine (`csilk_xdp_waf_add_ip_rule`), W3C trace context middleware with 2048-span ring buffer (`csilk_otlp_tracer_start_span`), and single-page embedded Web APM Dashboard (`share/csilk/apm_ui.html`, `/admin/apm`).

### Security
- **Sensitive buffer zeroing**: Zero sensitive buffers after use in csrf, jwt, session, and websocket modules to prevent data leakage.
- **JWT integer overflow guards**: Add overflow protection to base64 length calculations in JWT parsing.

### Fixed
- **bcrypt verify failure on empty passwords**: Fixed three bugs in the bcrypt implementation that caused `csilk_bcrypt_verify` to always return -1 for empty passwords: (1) `CSILK_BCRYPT_CIPHER_OUT` was 23 instead of 24 — 24 bytes encode to 32 base64 chars, but verify read only 31, dropping the last byte; (2) `datal`/`datar` were not zero-initialized before the Eksblowfish P-array keying loop (Step 2), causing salt to XOR against stack garbage for empty passwords; (3) `pwd_buf` was not `memset`-zeroed before `memcpy`, leaving uninitialized memory when `len == 0`. Updated `CSILK_BCRYPT_HASH_LEN` from 61 to 62 to match the corrected hash format (`$2a$XX$` + 22 salt + 32 checksum + NUL).
- **clang-tidy false positive**: Suppress `clang-analyzer-core.uninitialized.Assign` on `XL ^= p[i]` in `blowfish_encipher` — the analyzer cannot trace through the array-pointer parameter; the code is correct.
- **Compile warnings**: Fix `-Wcomment` (invalid `/*` inside block comment in connection.c), `-Wformat` in qdrant.c (use `%lld` for `int64_t`) and workflow_dsl.c (remove dead NULL arg from snprintf), apply clang-format to bcrypt signatures in crypto.h and crypto_dispatch.h.
- **Python wheel bundling**: Remove nested `csilk/` subdirectory path in `setup.py`; add `if(NOT DEFINED)` guards in CMakeLists.txt so `-D` values from setuptools are preserved.
- **macOS rpath**: Set `@loader_path` on shared library rpath for delocate-wheel compatibility.
- **Route macro safety**: Wrap route macros in `do { } while(0)` for safe use in control flow statements.
- **CI compatibility**: Bump upload/download-artifact to v6 for Node 24 support, skip FlameGraph upload when no samples collected, fix benchmark-results upload path.
- **macOS compatibility**: Add portable `explicit_bzero` shim for macOS builds.

### Changed
- **Include guards modernized**: Replace `#ifndef`/`#define` include guards with `#pragma once` across all 38 public headers.
- **API documentation**: Add Doxygen documentation to undocumented public API functions in middleware, server, and group headers.
- **tag-release.sh**: Extend to cover all version locations — `src/` `.c` `@version`, `python/csilk/_version.py`, `cmake/ports/csilk/vcpkg.json`, `vX.Y.Z+` doc headers, `| Version: X.Y.Z` metadata, ASCII diagram versions, and `version: X.Y.Z` code-block references.

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
- **Deferred cleanup API** (`csilk_ctx_defer` / `csilk_ctx_defer_free`): Panic-safe resource management. When `csilk_panic()` sets `panicked=1`, deferred callbacks run in LIFO order to release heap allocations, file descriptors, and mutex locks before the recovery middleware sends a 500 response.
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
