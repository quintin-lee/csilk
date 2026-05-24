# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.1] - 2026-05-24

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
