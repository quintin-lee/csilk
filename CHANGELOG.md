# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-05-23

### Added
- **Cookie Support**: Added `csilk_get_cookie` and `csilk_set_cookie` APIs for native cookie handling.
- **Multiple Headers**: Added `csilk_add_header` to support multiple response headers with the same key (e.g., `Set-Cookie`).
- **Build System**: Added `run_tests` custom target to CMake for easier test execution.
- **Documentation**: Added `CHANGELOG.md` and `CONTRIBUTING.md`.

### Changed
- **Security**: Hardened static file serving middleware with `realpath()` to prevent path traversal.
- **Resilience**: Implemented total request header size limit (`max_header_size`) to prevent memory-based attacks.
- **Stability**: Performed comprehensive memory allocation audit, ensuring all `malloc`/`calloc`/`strdup`/`realloc` calls are checked for success.
- **Concurrency**: Improved server shutdown mechanism using `uv_async_t` for thread-safe stopping.

### Fixed
- Fixed a bug where multiple request headers were not correctly parsed and committed in `src/server.c`.
- Fixed test suite failures in `test_recovery`, `test_timeout`, and `test_integration`.
- Fixed memory corruption issues caused by uninitialized context fields in tests.

## [0.1.0] - 2026-05-15

### Added
- Initial release with core routing, middleware, and server implementation.
- Support for JSON (cJSON), WebSocket, and YAML configuration.
- Built-in middleware: Logger, Recovery, Auth, CORS, CSRF, Rate Limiting, Static Files.
- Comprehensive Doxygen documentation.
