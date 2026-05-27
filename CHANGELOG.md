# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.3] - 2026-05-27

### Fixed
- **test_workflow_monitor SEGFAULT**: Fixed heap-buffer-overflow caused by
  `calloc(1, 1024)` — `csilk_ctx_t` is 2944 bytes, allocated buffer was too
  small. Under ASan this triggered a SEGFAULT on every CI run.
- **scaffold `csilk_perm_auto_middleware_passthrough`**: Replaced with
  existing `csilk_perm_auto_middleware` — the former never existed,
  causing compile failure in core API + perm mode.

### Changed
- **Header relocation**: `workflow_wal.h` moved from `src/app/` to
  `include/csilk/app/` to keep all headers under `include/`.

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
