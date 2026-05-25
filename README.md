# csilk

A lightweight, high-performance HTTP web framework written in C, inspired by Gin (Golang) and built on top of libuv, llhttp, and cJSON.


## Features

- 🚀 High performance using libuv for asynchronous I/O
- 🚀 **Zero-copy Static File Serving** via `sendfile` integration
- 📬 **Internal Event Bus** - Asynchronous, thread-safe Message Queue with middleware and subscriber support
- 📈 **Native Prometheus Metrics** - Built-in observability for QPS, latency, and status codes
- 🛡️ **Native HTTPS/TLS support** via OpenSSL integration
- 🔑 **JWT (JSON Web Token)** authentication middleware (HS256)
- 🔌 **Extensible Hook system** for lifecycle events (Server, Connection, Request)
- 🔧 **Pluggable Crypto Driver** for custom hashing and UUID algorithms
- 🔧 Middleware support (logger, recovery, auth, CORS, CSRF, rate limiting, static files)
- 🌐 RESTful API routing with parameter handling and route groups
- 📦 JSON support via cJSON (parse, serialize, error responses, reflection binding)
- 🍪 Cookie parsing and setting (with Max-Age, Secure, HttpOnly, etc.)
- 🔌 WebSocket support (RFC 6455 handshake, frame send/receive)
- 📡 Server-Sent Events (SSE) with csilk_sse_init/send/close
- 📦 Gzip response compression middleware (with intelligent skipping for media)
- 📤 Multipart/form-data file upload parsing
- 🔍 URL parsing and query string handling
- ⚡ Keep-alive connection support
- 🛡️ Graceful error handling with crash recovery (setjmp/longjmp)
- 📋 YAML configuration (server, logger, CORS, rate limit, static files, middleware)
- 🏗️ Arena allocator for request-scoped memory management
- 🗂️ Reflection engine for automatic struct <-> JSON conversion
- 🔐 Built-in CSRF protection, CORS, and rate limiting
- 📝 Complete Doxygen documentation for all public APIs and internals
- 🧵 Thread-safe logging with file rotation and ANSI colors
- 🔍 Configurable connection timeout and body/header size limits
- 🎯 Global (server-level) and per-route middleware support
- 🌲 Radix Tree router with :param and *wildcard matching
- 📝 Form URL-encoded body parsing (`application/x-www-form-urlencoded`)
- 🍪 Session management with **thread-safe mutex protection**
- 🔀 HTTP redirect helper (`csilk_redirect`)
- 📄 HTTP Range request support (206 Partial Content) for static files
- ✅ Request parameter validation middleware (required, int, string, email)
- 🆔 **Request ID middleware** for end-to-end tracing (X-Request-Id)
- 🩺 **Built-in Health Check** handler (/healthz)
- 📦 **Opaque Context API** for ABI stability

## Dependencies

- [libuv](https://github.com/libuv/libuv) - Asynchronous I/O library
- [llhttp](https://github.com/nodejs/llhttp) - HTTP parser
- [cJSON](https://github.com/DaveGamble/cJSON) - JSON parser
- [libyaml](https://github.com/yaml/libyaml) - YAML parser
- [OpenSSL](https://www.openssl.org/) - TLS/SSL and Cryptographic library (Required for HTTPS and JWT)

libuv and cJSON are automatically fetched during build via CMake's FetchContent. llhttp is used from the system if available, otherwise fetched. libyaml and OpenSSL must be installed as system dependencies.

### Installation (Debian/Ubuntu)
```bash
sudo apt install libyaml-dev libssl-dev
```

## Building

### Prerequisites

- CMake 3.11 or higher
- C compiler (supporting C11)
- Git (for fetching dependencies)
- libyaml development headers (`apt install libyaml-dev` on Debian/Ubuntu)

### Build Steps

```bash
# Clone the repository
git clone https://github.com/yourusername/csilk.git
cd csilk

# Create a build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
make

# By default, csilk builds as a static library.
# To build as a shared library, use:
# cmake .. -DBUILD_SHARED_LIBS=ON
#
# Other available options:
#   -DUSE_ASAN=ON          Enable AddressSanitizer (default OFF)
#   -DCSILK_BUILD_SHARED=ON Build shared library (default OFF)
#   -DUSE_FUZZER=ON        Enable libFuzzer (default OFF)

# Optional: Run tests
make run_tests     # Runs all tests via ctest
# or: ctest --test-dir . --output-on-failure

# Optional: Run individual test
./tests/test_logger
# ... other test executables

# Optional: Build documentation
make docs  # Requires Doxygen

# Optional: Format code
make format  # Requires clang-format
```

## Documentation

API documentation is written using **Doxygen** comments throughout the codebase. All public API functions, types, enums, and macros in the headers (`include/csilk.h`, `include/csilk_app.h`, `include/csilk_internal.h`, `include/csilk_reflect.h`) are fully documented with `@brief`, `@param`, and `@return` tags. Implementation files in `src/core/`, `src/app/`, and `src/middleware/` also carry complete Doxygen documentation with consistent `@copyright` and license annotations.

Online documentation is available at: **[https://quintin-lee.github.io/csilk/](https://quintin-lee.github.io/csilk/)**

To generate HTML documentation locally:

```bash
cd build && cmake .. && make docs     # Or: doxygen Doxyfile
```

Open `docs/html/index.html` in your browser to browse the API reference.

### Documentation Coverage

| Component | Status |
|-----------|--------|
| `include/csilk.h` (public API) | Fully documented |
| `include/csilk_app.h` (app API) | Fully documented |
| `include/csilk_internal.h` (internal API) | Fully documented |
| `include/csilk_reflect.h` (reflection API) | Fully documented |
| `src/core/` (core implementation) | Fully documented |
| `src/app/` (app layer) | Fully documented |
| `src/middleware/` (middleware) | Fully documented |
| `examples/` (example code) | Fully documented |

## Usage

### Simple Server Example

```c
#include "csilk.h"

void hello_handler(csilk_ctx_t* c) {
    csilk_string(c, 200, "Hello World!");
}

void ping_handler(csilk_ctx_t* c) {
    csilk_string(c, 200, "pong");
}

int main() {
    // Create router
    csilk_router_t* router = csilk_router_new();

    // Define routes
    csilk_handler_t hello_handlers[] = {hello_handler};
    csilk_router_add(router, "GET", "/", hello_handlers, 1);

    csilk_handler_t ping_handlers[] = {ping_handler};
    csilk_router_add(router, "GET", "/ping", ping_handlers, 1);

    // Create and run server
    csilk_server_t* server = csilk_server_new(router);
    csilk_server_run(server, 8080);

    // Cleanup (never reached in this example)
    csilk_server_free(server);
    csilk_router_free(router);

    return 0;
}
```

### Route Groups

```c
// Define handlers
void data_handler(csilk_ctx_t* c) {
    csilk_string(c, 200, "data response");
}

int main() {
    csilk_router_t* router = csilk_router_new();

    // Create a route group with "/api" prefix
    csilk_group_t* api = csilk_group_new(router, "/api");

    // Use convenience macros to add routes
    csilk_GET(api, "/data", data_handler);
    // -> matches GET /api/data

    // ...
}
```

### Middleware Usage

```c
void my_middleware(csilk_ctx_t* c) {
    // Custom logic before next handler
    csilk_next(c);
}

// Built-in middleware handlers:
//   csilk_logger_handler   - Request logging
//   csilk_recovery_handler - Panic recovery (500 on panic)
//   csilk_auth_middleware  - Token authentication (with validator callback)
//   csilk_static           - Static file serving (with root directory)

// Middleware is applied to route groups:
csilk_group_t* group = csilk_group_new(router, "/api");
csilk_group_use(group, csilk_logger_handler);
csilk_group_use(group, csilk_recovery_handler);
```

### HTTPS & JWT Example

```c
#include "csilk.h"

void secure_handler(csilk_ctx_t* c) {
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    const char* user = cJSON_GetObjectItem(payload, "sub")->valuestring;
    char msg[64];
    snprintf(msg, sizeof(msg), "Hello secure user: %s", user);
    csilk_string(c, 200, msg);
}

int main() {
    csilk_router_t* router = csilk_router_new();
    csilk_group_t* api = csilk_group_new(router, "/api");
    
    // Apply JWT middleware to /api group
    csilk_group_use(api, (csilk_handler_t)csilk_jwt_middleware, "secret_key");
    csilk_GET(api, "/profile", secure_handler);

    csilk_server_t* server = csilk_server_new(router);
    
    // Configure HTTPS
    csilk_server_config_t config = {0};
    config.enable_tls = 1;
    config.tls_cert_file = "cert.pem";
    config.tls_key_file = "key.pem";
    csilk_server_set_config(server, &config);

    csilk_server_run(server, 443);
    return 0;
}
```

## Project Structure

```
src/
  ├── core/          # Core engine
  │   ├── context.c       # Request/response context
  │   ├── router.c        # Routing implementation (Radix Tree)
  │   ├── group.c         # Route groups
  │   ├── server.c        # HTTP server implementation (libuv + llhttp)
  │   ├── arena.c         # Arena memory allocator
  │   ├── config.c        # YAML configuration loader + validation
  │   ├── logger.c        # Thread-safe logging with rotation
  │   ├── mq.c            # Internal Event Bus (Message Queue)
  │   ├── url.c           # URL / query string parsing
  │   ├── utils.c         # SHA1 hashing and Base64 encoding
  │   ├── websocket.c     # WebSocket handshake and frame handling
  │   └── reflect.c       # Reflection engine (JSON <-> struct)
  ├── app/            # High-level app wrapper
  │   └── app.c           # csilk_app_t — Express-like convenience API
  └── middleware/     # Built-in middleware
      ├── auth.c      # Token-based authentication
      ├── cors.c      # Cross-Origin Resource Sharing
      ├── csrf.c      # CSRF protection
      ├── gzip.c      # Gzip response compression
      ├── logger.c    # Request logging
      ├── metrics.c   # Prometheus metrics
      ├── multipart.c # Multipart/form-data parsing
      ├── ratelimit.c # IP-based rate limiting
      ├── recovery.c  # Panic recovery
      ├── session.c   # Cookie-based session management
      ├── sse.c       # Server-Sent Events
      ├── static.c    # Static file serving (with Range/206 support)
      └── validate.c  # Request parameter validation

include/              # Public headers
  ├── csilk.h             # Main framework header
  ├── csilk_app.h         # csilk_app_t convenience API
  ├── csilk_internal.h    # Internal utilities
  └── csilk_reflect.h     # Reflection engine
tests/                # Unit tests
examples/             # Advanced usage examples
examples/example_server.c      # Full-featured demo server
examples/example_app.c         # Minimal app API demo
examples/advanced_server.c     # Advanced config-driven server example
```

## Testing

The project includes a comprehensive test suite. After building, run individual test executables:

```bash
./tests/test_context
./tests/test_router
./tests/test_server
# ... and others
```

### Features Legend

| Emoji | Meaning |
|-------|---------|
| 🚀 | Performance / Async I/O |
| 📬 | Internal Event Bus (MQ) |
| 📈 | Prometheus Metrics |
| 🔧 | Middleware / Tooling |
| 🌐 | Networking / Routing |
| 📦 | JSON / Data serialization |
| 🍪 | Cookie management |
| 🔌 | WebSocket support |
| 📡 | Server-Sent Events (SSE) |
| 📤 | File upload / Multipart |
| 🔍 | URL / Query parsing |
| ⚡ | Connection keep-alive |
| 🛡️ | Error handling / Security |
| 📋 | Configuration (YAML) |
| 🏗 | Memory management (Arena) |
| 🗂️ | Reflection engine |
| 🔐 | CSRF / CORS / Rate limiting |
| 📝 | Documentation (Doxygen) |
| 🧵 | Thread-safe logging |
| 🔍 | Timeouts / Limits |
| 🎯 | Per-route middleware |
| 🌲 | Radix Tree router |
| 📝 | Form URL-encoded parsing |
| 🍪 | Session management |
| 🔀 | HTTP redirect |
| 📄 | HTTP Range / 206 Partial Content |
| ✅ | Parameter validation |

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Inspired by [Gin](https://github.com/gin-gonic/gin) web framework
- Built upon excellent C libraries: libuv, llhttp, and cJSON