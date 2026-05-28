# Getting Started

## Prerequisites

- CMake 3.11+
- C compiler (C11 support)
- Git
- libyaml-dev
- zlib1g-dev (for gzip middleware)
- libssl-dev (for HTTPS/TLS, JWT, cipher drivers)
- libcurl-dev (for AI driver HTTP transport)
- libuv (auto-fetched via CMake if not present)
- Optional: libmysqlclient-dev, libpq-dev, libmongoc-dev (for database drivers)

## Build & Dependency Flow

```mermaid
flowchart LR
    subgraph Host System
        CC["C Compiler (C11)"]
        SYS["libyaml-dev\nzlib1g-dev\npthread"]
    end

    subgraph "CMake FetchContent"
        UV["libuv v1.48.0"]
        LL["llhttp v9.4.1"]
        CJ["cJSON v1.7.18"]
    end

    subgraph Build
        CMAKE["CMakeLists.txt"]
        OBJ["Object Files\nsrc/*.c"]
        LIB["libcsilk.a\nlibcsilk.so"]
        EXE["example_server\nexample_app\ntests/*"]
    end

    CC --> OBJ
    SYS --> OBJ
    UV --> OBJ
    LL --> OBJ
    CJ --> OBJ
    CMAKE --> OBJ
    OBJ --> LIB
    OBJ --> EXE
    LIB --> EXE
```

## Build from Source

```bash
git clone https://github.com/username/csilk.git
cd csilk
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | - | `Debug`, `Release`, `RelWithDebInfo` |
| `CSILK_BUILD_SHARED` | OFF | Build shared library (`libcsilk.so`) |
| `USE_ASAN` | OFF | Enable AddressSanitizer |
| `USE_FUZZER` | OFF | Build fuzz test harness |
| `CSILK_USE_MYSQL` | OFF | Enable MySQL database driver |
| `CSILK_USE_POSTGRES` | OFF | Enable PostgreSQL database driver |
| `CSILK_USE_MONGODB` | OFF | Enable MongoDB database driver |

## Create a New Project

Csilk provides a scaffolding tool `csilkskel` to quickly generate a new project with a professional, layered architecture and built-in Swagger UI and Admin Dashboard.

```bash
# Clone the repository
git clone https://github.com/username/csilk.git
cd csilk

# Generate a new project
bash scripts/csilkskel -n my-service

# Build and run the new project
cd my-service
mkdir build && cd build
cmake ..
make
./my-service
```

The generated project includes:
- **Layered Architecture**: Dedicated directories for API handlers, service logic, and data models.
- **Interactive Documentation**: Built-in Swagger UI available at `http://localhost:8080/`.
- **Admin Dashboard**: Real-time monitoring at `http://localhost:8080/admin/`.
- **Reflection Example**: A complete User service demonstrating automatic JSON binding.

## Run Example
...

```bash
# Low-level API demo
./build/example_server

# High-level app API demo
./build/example_app
```

## Example Server Walkthrough

```mermaid
sequenceDiagram
    participant Main
    participant Router as csilk_router_t
    participant Group as csilk_group_t
    participant Server as csilk_server_t
    participant EvLoop as libuv Event Loop

    Main->>Router: csilk_router_new()
    Main->>Group: csilk_group_new(router, "/api")
    Main->>Group: csilk_GET(group, "/ping", handler)
    Main->>Server: csilk_server_new(router)
    Main->>Server: csilk_server_use(server, cors_middleware)
    Main->>Server: csilk_server_set_config(server, config)
    Main->>Server: csilk_server_run(server, 8080)
    Server->>EvLoop: uv_run() blocks main thread
    Note over EvLoop: Accepting connections...
```

## Running Tests

```bash
cd build && ctest --output-on-failure
```

## Minimal Server Program

```c
#include "csilk/csilk.h"

void ping(csilk_ctx_t* c) {
    csilk_string(c, 200, "pong");
}

int main() {
    csilk_router_t* r = csilk_router_new();
    csilk_router_add(r, "GET", "/ping", (csilk_handler_t[]){ping, NULL}, 1);

    csilk_server_t* s = csilk_server_new(r);
    csilk_server_run(s, 8080);

    csilk_router_free(r);
    csilk_server_free(s);
    return 0;
}
```
