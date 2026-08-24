# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Public Cipher API**: Added `csilk_symmetric_encrypt/decrypt` (AES-256-GCM), `csilk_rsa_generate_keypair`, `csilk_rsa_encrypt/decrypt`, `csilk_rsa_sign/verify` in `<csilk/core/cipher.h>` — standalone operations without request context.
- **Public HTTP/2 API** (`csilk/http/h2.h`): Promoted from internal `src/core/http/h2.h` to public `include/csilk/http/h2.h`; now includes `csilk_h2_init_session`, `csilk_h2_process_data`, `csilk_h2_get_or_create_stream`, `csilk_h2_free_streams`, `csilk_h2_remove_stream`, `csilk_h2_send_response`, `csilk_h2_submit_push`.
- **Public Flame Graph API** (`csilk/util/flamegraph.h`): Promoted from internal `src/util/flamegraph.h` to public `include/csilk/util/flamegraph.h`; now includes `csilk_flamegraph_start`, `csilk_flamegraph_stop`, `csilk_flamegraph_is_running`.

### Changed
- **Crypto module refactoring**: Split 711-line `src/crypto/crypto.c` into `crypto.c` (primitives: SHA-256, HMAC, UUID, RNG, nonce, ~297 lines) and `src/crypto/cipher_dispatch.c` (cipher dispatch: AES/RSA/JWT, ~350 lines). Moved `src/crypto/url.c` to `src/core/primitives/url.c` (HTTP parsing utility, not cryptography).
- **Include directory alignment**: All public headers now mirror the src/ module layout. Internal-only headers (`header_map.h`, `query.h`, `lfqueue.h`) remain in `src/` only.
- **Test count**: 211 → 213 (added cipher public API tests: 5 test functions).

### Fixed
- **clang-tidy**: Zero warnings on all changed files.

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
