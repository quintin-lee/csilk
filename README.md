# c-gin-framework

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
git clone https://github.com/yourusername/c-gin-framework.git
cd c-gin-framework

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

API documentation is written using **Doxygen** comments throughout the codebase. All public API functions, types, enums, and macros in the headers (`include/gin.h`, `include/gin_internal.h`) are fully documented with `@brief`, `@param`, and `@return` tags. Implementation files in `src/` and `src/middleware/` also carry complete Doxygen documentation.

To generate HTML documentation:

```bash
cd build && cmake .. && make docs     # Or: doxygen Doxyfile
```

Open `docs/html/index.html` in your browser to browse the API reference.

### Documentation Coverage

| Component | Status |
|-----------|--------|
| `include/gin.h` (public API) | Fully documented |
| `include/gin_internal.h` (internal API) | Fully documented |
| `src/` (core implementation) | Fully documented |
| `src/middleware/` (middleware) | Fully documented |

## Usage

### Simple Server Example

```c
#include "gin.h"

void hello_handler(gin_ctx_t* c) {
    gin_string(c, 200, "Hello World!");
}

void ping_handler(gin_ctx_t* c) {
    gin_string(c, 200, "pong");
}

int main() {
    // Create router
    gin_router_t* router = gin_router_new();

    // Define routes
    gin_handler_t hello_handlers[] = {hello_handler};
    gin_router_add(router, "GET", "/", hello_handlers, 1);

    gin_handler_t ping_handlers[] = {ping_handler};
    gin_router_add(router, "GET", "/ping", ping_handlers, 1);

    // Create and run server
    gin_server_t* server = gin_server_new(router);
    gin_server_run(server, 8080);

    // Cleanup (never reached in this example)
    gin_server_free(server);
    gin_router_free(router);

    return 0;
}
```

### Route Groups

```c
// Define handlers
void data_handler(gin_ctx_t* c) {
    gin_string(c, 200, "data response");
}

int main() {
    gin_router_t* router = gin_router_new();

    // Create a route group with "/api" prefix
    gin_group_t* api = gin_group_new(router, "/api");

    // Use convenience macros to add routes
    gin_GET(api, "/data", data_handler);
    // -> matches GET /api/data

    // ...
}
```

### Middleware Usage

```c
void my_middleware(gin_ctx_t* c) {
    // Custom logic before next handler
    gin_next(c);
}

// Built-in middleware handlers:
//   gin_logger_handler   - Request logging
//   gin_recovery_handler - Panic recovery (500 on panic)
//   gin_auth_middleware  - Token authentication (with validator callback)
//   gin_static           - Static file serving (with root directory)

// Middleware is applied to route groups:
gin_group_t* group = gin_group_new(router, "/api");
gin_group_use(group, gin_logger_handler);
gin_group_use(group, gin_recovery_handler);
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