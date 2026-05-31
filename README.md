# csilk

A lightweight, high-performance HTTP web framework written in C, inspired by Gin (Golang) and built on top of libuv, llhttp, nghttp2, and cJSON.


## Features

- 🚀 High performance using libuv for asynchronous I/O
- 🚀 **Zero-copy Static File Serving** via `sendfile` integration
- 📬 **Internal Event Bus** - Asynchronous, thread-safe Message Queue with middleware and subscriber support
- 📈 **Native Prometheus Metrics** - Built-in observability for QPS, latency, and status codes
- 🖥️ **Unified Admin Dashboard** - Web-based real-time monitoring of HTTP, AI Workflows, MQ, and CPU flame graphs
- 🔥 **Real-time CPU Flame Graph** — Backtrace sampling for performance profiling in admin dashboard
- 🛡️ **Native HTTPS/TLS support** via OpenSSL integration
- 🌐 **HTTP/2 support** via nghttp2 (ALPN negotiation, multiplexing, HPACK, Server Push)
- ⚡ **SIMD-accelerated routing** — AVX2 (x86_64) and NEON (aarch64) path matching
- 🔑 **JWT (JSON Web Token)** authentication middleware (HS256)
- 🔌 **Extensible Hook system** for lifecycle events (Server, Connection, Request)
- 🔧 **Pluggable Crypto Driver** for custom hashing and UUID algorithms
- 🔐 **Pluggable Cipher Driver** for AES-256-GCM, RSA-OAEP, and RSA-PSS
- 🗄️ **Pluggable Database Drivers** - SQLite, MySQL, PostgreSQL, MongoDB, Redis
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
- 🤖 **Unified AI Interface** - Provider-agnostic API for Chat, Embeddings, and Tool Calling (OpenAI & Ollama)
- 🔧 **Dynamic AI Tool Discovery** — MCP-like tool discovery for agentic workflows
- 🗂️ Reflection engine for automatic struct <-> JSON conversion (including basic types and arrays)
- 🧬 **OpenAPI Spec Generation** — Auto-generate OpenAPI 3.0 JSON from route metadata
- 🧬 **SDK Code Generation** — Auto-generate TypeScript and Python API clients from OpenAPI spec
- 🔐 Built-in CSRF protection, CORS, rate limiting, and WAF
- 📝 Complete Doxygen documentation for all public APIs and internals
- 🧵 Thread-safe logging with file rotation and ANSI colors
- 🔍 Configurable connection timeout and body/header size limits
- 🎯 Global (server-level), group-level, and per-route middleware support
- 🌲 Radix Tree router with :param and *wildcard matching
- 📝 Form URL-encoded body parsing (`application/x-www-form-urlencoded`)
- 🍪 Session management with **thread-safe mutex protection**
- 🔀 HTTP redirect helper (`csilk_redirect`)
- 📄 HTTP Range request support (206 Partial Content) for static files
- ✅ Request parameter validation middleware (required, int, string, email)
- 🆔 **Request ID middleware** for end-to-end tracing (X-Request-Id)
- 🩺 **Built-in Health Check** handler (/healthz)
- 📦 **Opaque Context API** for ABI stability
- 🍏 **macOS 14 ARM64** — CI-supported build target
- 🔒 **Lock-free per-worker connection pool** for multi-core scaling
- 🔄 **Hot Reload** — Swap router at runtime without restart
- 🧹 **Deferred Cleanup API** (`csilk_ctx_defer`) — panic-safe resource management

## Dependencies

- [libuv](https://github.com/libuv/libuv) - Asynchronous I/O library
- [llhttp](https://github.com/nodejs/llhttp) - HTTP/1.1 parser
- [nghttp2](https://github.com/nghttp2/nghttp2) - HTTP/2 library
- [cJSON](https://github.com/DaveGamble/cJSON) - JSON parser
- [libyaml](https://github.com/yaml/libyaml) - YAML parser
- [OpenSSL](https://www.openssl.org/) - TLS/SSL and Cryptographic library
- [zlib](https://www.zlib.net/) - Gzip compression
- [libcurl](https://curl.se/libcurl/) - HTTP client (AI drivers)
- [sqlite3](https://www.sqlite.org/) - Embedded SQL database

libuv, nghttp2, and cJSON are automatically fetched during build via CMake's FetchContent. llhttp is used from the system if available, otherwise fetched. libyaml, OpenSSL, zlib, libcurl, and sqlite3 must be installed as system dependencies.

### Installation (Debian/Ubuntu)
```bash
sudo apt install libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev libsqlite3-dev
```

## Supported Platforms

csilk requires **C23** language support (`static constexpr`, `nullptr`, `bool` keywords).

### Compilers

| Compiler        | Minimum Version | Notes                                                    |
|-----------------|:--------------:|----------------------------------------------------------|
| **GCC**         | 13+            | Full C23 support. Primary CI target on Ubuntu 24.04.     |
| **Clang**       | 19+            | C23 `constexpr` support. libFuzzer fuzzing.              |
| **Apple Clang** | —              | Not supported — lacks C23 `constexpr` and `nullptr`.     |
| **MSVC**        | —              | Not supported — POSIX APIs (libuv, pthread, sys/socket). |

### Operating Systems

| Platform          | Status      | Notes                                                       |
|-------------------|:-----------:|-------------------------------------------------------------|
| **Linux**         | Supported   | Ubuntu 24.04 (CI), Debian 12+, any glibc-based distro.      |
| **macOS**         | Planned     | Missing `pthread_barrier_t` in multi-worker mode.           |
| **Windows**       | Not planned | POSIX dependency surface too large (libuv may enable).      |
| **musl / Alpine** | Untested    | Likely compatible; no CI coverage.                          |

### Dependency Versions

| Dependency     | Minimum   | Purpose                        |
|----------------|:---------:|--------------------------------|
| CMake          | 3.11      | Build system                   |
| OpenSSL        | 1.1.1     | TLS, crypto, JWT (HS256)       |
| libcurl        | 7.80.0    | HTTP client (AI drivers)       |
| libyaml        | 0.2.0     | Configuration parsing          |
| zlib           | 1.2.0     | Gzip compression               |
| sqlite3        | 3.20.0    | Embedded database              |
| pthread        | —         | Threading (system)             |

## Building

### Prerequisites

- CMake 3.11 or higher
- C compiler (supporting C23)
- Git (for fetching dependencies)
- System dependencies: `sudo apt install libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev libsqlite3-dev`

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

### Docker

```bash
# Build the Docker image
docker build -t csilk .

# Run a container
docker run -p 8080:8080 csilk

# Override config
docker run -p 8080:8080 -v $(pwd)/custom_config.yaml:/etc/csilk/config.yaml csilk
```

## Documentation

API documentation is written using **Doxygen** comments throughout the codebase. All public API functions, types, enums, and macros in the headers (`include/csilk/csilk.h`, `include/csilk/app/app.h`, `include/csilk/core/internal.h`, `include/csilk/reflection/reflect.h`) are fully documented with `@brief`, `@param`, and `@return` tags. Implementation files in `src/` subdirectories also carry complete Doxygen documentation with consistent `@copyright` and license annotations.

Online documentation is available at: **[https://quintin-lee.github.io/csilk/](https://quintin-lee.github.io/csilk/)**

To generate HTML documentation locally:

```bash
cd build && cmake .. && make docs     # Or: doxygen Doxyfile
```

Open `docs/html/index.html` in your browser to browse the API reference.

### Documentation Coverage

| Component | Status |
|-----------|--------|
| `include/csilk/` (public API hierarchy) | Fully documented |
| `src/core/` (kernel implementation) | Fully documented |
| `src/app/` (app layer) | Fully documented |
| `src/ai/`, `src/data/`, `src/security/` | Fully documented |
| `src/middleware/` (middleware) | Fully documented |
| `examples/` (example code) | Fully documented |

## Usage

### Simple Server Example

```c
#include "csilk/csilk.h"

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
#include "csilk/csilk.h"

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
  ├── core/           # Kernel (libuv TCP, Router, Arena, Logger, Config)
  ├── app/            # Application Layer (app, admin dashboard, workflow engine)
  ├── ai/             # AI Unified Interface Engine
  ├── data/           # Database Abstraction Layer
  ├── messaging/      # Internal Event Bus (Message Queue)
  ├── security/       # Permission & Security Core
  ├── reflection/     # Reflection Engine implementation
  ├── protocols/      # Protocol Extensions (WebSocket, Swagger)
  ├── drivers/        # Concrete Drivers (OpenAI, Ollama, SQLite, MySQL, PostgreSQL, MongoDB, Redis, Qdrant)
  └── middleware/     # 15 built-in middleware modules

include/csilk/        # Public Hierarchical Headers
  ├── core/           # Core internal definitions
  ├── app/            # App API, Admin, Workflow, WAL
  ├── drivers/        # Driver interfaces (AI, Cipher, DB, Perm)
  ├── reflection/     # Reflection engine API
  ├── test/           # OOM simulation test framework
  └── csilk.h         # Main entry point (includes all modules)

tests/                # 118 comprehensive unit tests
examples/             # Functional usage examples
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
| 🌐 | Networking / Routing / HTTP/2 |
| 🔧 | Middleware / Tooling |
| 📦 | JSON / Data serialization |
| 🍪 | Cookie management |
| 🔌 | WebSocket support |
| 📡 | Server-Sent Events (SSE) |
| 📤 | File upload / Multipart |
| 🔍 | URL / Query parsing |
| ⚡ | Connection keep-alive |
| 🛡️ | Error handling / Security |
| 🔐 | Crypto / Cipher drivers |
| 📋 | Configuration (YAML) |
| 🏗 | Memory management (Arena) |
| 🗂️ | Reflection engine |
| 🤖 | AI Unified Interface |
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
- Built upon excellent C libraries: libuv, llhttp, nghttp2, and cJSON