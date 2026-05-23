# csilk

A lightweight, high-performance HTTP web framework written in C, inspired by Gin (Golang) and built on top of libuv, llhttp, and cJSON.

## Features

- 🚀 High performance using libuv for asynchronous I/O
- 🔧 Middleware support (logger, recovery, auth, CORS, CSRF, rate limiting, static files)
- 🌐 RESTful API routing with parameter handling and route groups
- 📦 JSON support via cJSON
- 🔌 WebSocket support
- 🔍 URL parsing and query string handling
- ⚡ Keep-alive connection support
- 🛡️ Graceful error handling
- 📝 Complete Doxygen documentation for all public APIs and internals

## Dependencies

- [libuv](https://github.com/libuv/libuv) - Asynchronous I/O library
- [llhttp](https://github.com/nodejs/llhttp) - HTTP parser
- [cJSON](https://github.com/DaveGamble/cJSON) - JSON parser

These dependencies are automatically fetched during build via CMake's FetchContent.

## Building

### Prerequisites

- CMake 3.11 or higher
- C compiler (supporting C11)
- Git (for fetching dependencies)

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

# Optional: Run tests
./tests/test_logger
./tests/test_router
# ... other test executables

# Optional: Build documentation
make docs  # Requires Doxygen

# Optional: Format code
make format  # Requires clang-format
```

## Documentation

API documentation is written using **Doxygen** comments throughout the codebase. All public API functions, types, enums, and macros in the headers (`include/csilk.h`, `include/csilk_internal.h`) are fully documented with `@brief`, `@param`, and `@return` tags. Implementation files in `src/` and `src/middleware/` also carry complete Doxygen documentation.

To generate HTML documentation:

```bash
cd build && cmake .. && make docs     # Or: doxygen Doxyfile
```

Open `docs/html/index.html` in your browser to browse the API reference.

### Documentation Coverage

| Component | Status |
|-----------|--------|
| `include/csilk.h` (public API) | Fully documented |
| `include/csilk_internal.h` (internal API) | Fully documented |
| `src/` (core implementation) | Fully documented |
| `src/middleware/` (middleware) | Fully documented |

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

## Project Structure

```
src/
  ├── context.c       # Request/response context
  ├── router.c        # Routing implementation (Radix Tree)
  ├── group.c         # Route groups
  ├── server.c        # HTTP server implementation (libuv + llhttp)
  ├── arena.c         # Arena memory allocator
  ├── config.c        # YAML configuration loader
  ├── logger.c        # Thread-safe logging with rotation
  ├── utils.c         # SHA1 hashing and Base64 encoding
  ├── url_parser.c    # URL parsing utilities
  ├── websocket.c     # WebSocket handshake and frame handling
  └── middleware/     # Built-in middleware
      ├── auth.c      # Token-based authentication
      ├── cors.c      # Cross-Origin Resource Sharing
      ├── csrf.c      # CSRF protection
      ├── logger.c    # Request logging
      ├── ratelimit.c # IP-based rate limiting
      ├── recovery.c  # Panic recovery
      └── static.c    # Static file serving

include/              # Public headers
tests/                # Unit tests
examples/             # Advanced usage examples
example_server.c      # Simple example server
```

## Testing

The project includes a comprehensive test suite. After building, run individual test executables:

```bash
./tests/test_context
./tests/test_router
./tests/test_server
# ... and others
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Inspired by [Gin](https://github.com/gin-gonic/gin) web framework
- Built upon excellent C libraries: libuv, llhttp, and cJSON