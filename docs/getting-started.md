# Getting Started

## Prerequisites

- CMake 3.11+ (**MUST** be available in `$PATH`)
- C compiler with C23 support (GCC 13+ or Clang 19+)
- Git
- libyaml-dev (**MUST** for YAML configuration parsing)
- zlib1g-dev (for gzip compression middleware — **SHOULD** be enabled for production)
- libssl-dev (**MUST** be OpenSSL 1.1.1+ for HTTPS/TLS, JWT, cipher drivers)
- libcurl-dev 7.80.0+ (**MUST** for AI driver HTTP transport)
- libuv (auto-fetched via CMake FetchContent for libuv backend)
- liburing (auto-fetched via CMake FetchContent when `-DCSILK_USE_URING=ON`)
- Optional: libmysqlclient-dev, libpq-dev, libmongoc-dev (for database drivers — enable via `-DCSILK_USE_*` CMake flags)

## Build & Dependency Flow

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
flowchart LR
    subgraph Host_System["fa:fa-laptop Host System"]
        CC["fa:fa-code C Compiler (C23)"]
        SYS["fa:fa-cogs libyaml-dev\nzlib1g-dev\npthread"]
    end

    subgraph FetchContent["fa:fa-download CMake FetchContent"]
        UV["fa:fa-sync-alt libuv v1.48.0 (default)"]
        URING["fa:fa-tachometer liburing v2.5 (optional, -DCSILK_USE_URING=ON)"]
        LL["fa:fa-file-code llhttp v9.4.1"]
        CJ["fa:fa-file-code cJSON v1.7.18"]
    end

    subgraph build["fa:fa-hammer Build"]
        CMAKE["fa:fa-file-alt CMakeLists.txt"]
        OBJ["fa:fa-cubes Object Files\nsrc/*.c"]
        LIB["fa:fa-archive libcsilk.a\nlibcsilk.so"]
        EXE["fa:fa-play example_server\nexample_app\ntests/*"]
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
| `CSILK_USE_URING` | OFF | Use io_uring backend instead of libuv (Linux-only) |
| `CMAKE_BUILD_TYPE` | - | `Debug`, `Release`, `RelWithDebInfo` |
| `CSILK_BUILD_SHARED` | OFF | Build shared library (`libcsilk.so`) |
| `USE_ASAN` | OFF | Enable AddressSanitizer |
| `USE_FUZZER` | OFF | Build fuzz test harness |
| `USE_COVERAGE` | OFF | Enable gcov coverage reporting |
| `CSILK_USE_MYSQL` | OFF | Enable MySQL database driver |
| `CSILK_USE_POSTGRES` | OFF | Enable PostgreSQL database driver |
| `CSILK_USE_MONGODB` | OFF | Enable MongoDB database driver |
| `ENABLE_OOM_TEST` | OFF | Enable out-of-memory simulation tests |

## Create a New Project

Csilk provides a scaffolding tool `csilkskel` to quickly generate a new project with a professional, layered architecture and built-in Swagger UI and Admin Dashboard.

```bash
# Generate a new project (interactive Python tool)
python3 scripts/csilkskel -n my-service

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
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  }
}}%%
sequenceDiagram
    participant Main as fa:fa-user Main
    participant Router as fa:fa-sitemap csilk_router_t
    participant Group as fa:fa-folder csilk_group_t
    participant Server as fa:fa-server csilk_server_t
    participant EvLoop as fa:fa-sync-alt libuv Event Loop

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

## Python Bindings Quickstart

If you prefer Python, you can write `csilk` applications in Python using the `ctypes` wrapper package:

1. Compile the `csilk` shared library:
```bash
cmake .. -DCSILK_BUILD_SHARED=ON
make
```

2. Install the python package in development mode:
```bash
pip install -e ./python
```

3. Create a simple `app.py` script:
```python
from csilk import App, Context

app = App()

@app.get("/ping")
def ping(ctx: Context):
    ctx.string(200, "pong")

if __name__ == "__main__":
    app.run(8080)
```

4. Run the application:
```bash
python3 app.py
```
