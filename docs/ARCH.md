# csilk Architecture Whitepaper

> **Last updated**: 2026-05-31 | **Version**: 0.3.0

## 1. Core Architecture Design

csilk adopts the classic **Reactor Event-Driven Model**, combined with an **Onion Model** middleware mechanism inspired by Go's Gin framework.

### 1.1 System Architecture Overview

```mermaid
graph TB
    subgraph External
        CLI["HTTP Clients<br/>(browsers, curl, SDKs)"]
    end

    subgraph "csilk Framework"
        subgraph "Transport Layer"
            TCP["TCP Server (libuv)"]
            TLS["OpenSSL (TLS v1.2/v1.3)"]
            H2DISPATCH["Protocol Dispatcher<br/>(ALPN: h2 vs http/1.1)"]
            H1["HTTP/1.1 Parser (llhttp)"]
            H2["HTTP/2 Parser (nghttp2)"]
        end

        subgraph "Core Engine"
            SRV["csilk_server_s<br/>Event Loop + Config + Global MW"]
            RTR["Radix Tree Router<br/>Static + :param + *wildcard"]
            GRP["Group Router<br/>Hierarchical prefix + MW"]
            CTX["csilk_ctx_s<br/>Request/Response State Machine"]
            ARENA["Arena Allocator<br/>Bump allocator per-connection"]
            MQ["Message Queue<br/>Thread-safe Event Bus"]
        end

        subgraph "Middleware Chain"
            REC["Recovery<br/>(setjmp/longjmp)"]
            LOG["Logger<br/>(structured JSON)"]
            MET["Metrics<br/>(Prometheus/Observability)"]
            WAF["WAF<br/>(SQLi/XSS/Path Traversal)"]
            AUTH["Auth<br/>(token validation)"]
            CORS["CORS<br/>(cross-origin)"]
            RATE["RateLimit<br/>(sliding window)"]
            CSRF["CSRF<br/>(token anti-forgery)"]
            STATIC["Static Files<br/>(thread pool I/O)"]
            GZIP["Gzip<br/>(zlib + thread pool)"]
            SSE["SSE<br/>(Server-Sent Events)"]
            MPART["Multipart<br/>(form-data parser)"]
        end

        subgraph "Protocol Upgrade"
            WS["WebSocket Engine<br/>Handshake + Frame + Close"]
        end
    end

    subgraph "Dependencies"
        UV["libuv v1.48<br/>Event Loop + Thread Pool"]
        LLHTTP["llhttp v9.4<br/>Streaming HTTP Parser"]
        NGHTTP2["nghttp2 v1.52<br/>HTTP/2 Frame Parser"]
        CJ["cJSON v1.7<br/>JSON Parse/Serialize"]
        YAML["libyaml<br/>Config Parse"]
        ZLIB["zlib<br/>Gzip Compression"]
        SSL["OpenSSL<br/>TLS + Crypto"]
    end

    CLI --> TCP
    TCP --> TLS
    TLS --> SSL
    TLS --> H2DISPATCH
    H2DISPATCH --> H1
    H2DISPATCH --> H2
    H1 --> SRV
    H2 --> SRV
    SRV --> RTR
    SRV --> GRP
    SRV --> CTX
    SRV --> REC
    SRV --> LOG
    SRV --> AUTH
    SRV --> CORS
    SRV --> RATE
    SRV --> CSRF
    SRV --> STATIC
    SRV --> GZIP
    SRV --> SSE
    SRV --> MPART
    CTX --> ARENA
    SRV --> WS
    SRV --> UV
    HTTP --> LLHTTP
    GZIP --> ZLIB
    STATIC --> ZLIB
    WS --> CJ
    SRV --> YAML
```

### 1.2 Event-Driven Base

The framework is built on `libuv`:
- All network I/O is non-blocking.
- A **Protocol Dispatcher** selects between `llhttp` (HTTP/1.1) and `nghttp2` (HTTP/2) based on TLS ALPN negotiation (`h2` vs `http/1.1`).
- Uses `llhttp` for state-machine-driven HTTP/1.1 protocol parsing, ensuring high parsing efficiency and low memory footprint.
- Uses `nghttp2` for HTTP/2 binary frame parsing, HPACK header compression, and stream multiplexing.

```mermaid
sequenceDiagram
    participant K as Kernel
    participant UV as libuv Event Loop
    participant TCP as TCP Handle
    participant LL as llhttp Parser
    participant S as Server

    K-->>UV: epoll/kqueue: new connection ready
    UV->>S: on_new_connection()
    S->>TCP: uv_tcp_init() + uv_accept()
    S->>LL: llhttp_init(parser, HTTP_REQUEST, settings)
    S->>UV: uv_read_start(stream, alloc_buffer, on_read)

    Note over UV: Event loop idle, waiting

    K-->>UV: epoll/kqueue: data available
    UV->>S: on_read()
    S->>LL: llhttp_execute(parser, buf, nread)

    LL-->>S: on_url(data, len)
    LL-->>S: on_header_field(data, len)
    LL-->>S: on_header_value(data, len)
    LL-->>S: on_headers_complete()
    LL-->>S: on_body(data, len)
    LL-->>S: on_message_complete()

    S->>S: finalize_request() + csilk_router_match_ctx()
    S->>S: Assemble handler chain + csilk_next()

    S->>S: _csilk_send_response() → uv_write()
    S->>UV: uv_read_start() (keep-alive)
```

> **HTTP/2** (ALPN-negotiated):  
> The protocol dispatcher routes TLS-decrypted data to `csilk_h2_process_data()` and `nghttp2` instead of `llhttp`. The `nghttp2` session callbacks (`on_header_callback`, `on_frame_recv_callback`, etc.) populate a `csilk_ctx_t` per stream ID and invoke the same `_csilk_dispatch_request()` path, sharing the router, middleware chain, and hook system with HTTP/1.1.

### 1.3 Onion Model Middleware

Middleware implements bidirectional interception through the `csilk_next()` mechanism:

```mermaid
flowchart LR
    REQ["Request"]

    subgraph "Onion Layers"
        direction LR
        L1i["Recovery (pre)<br/>setjmp()"] --> L2i["Logger (pre)<br/>start timer"]
        L2i --> L3i["Auth (pre)<br/>check token"]
        L3i --> L4i["RateLimit (pre)<br/>check quota"]
        L4i --> CORE["Business Handler<br/>(csilk_string/json)"]
        CORE --> L4o["RateLimit (post)<br/>(no-op)"]
        L4o --> L3o["Auth (post)<br/>(no-op)"]
        L3o --> L2o["Logger (post)<br/>log latency"]
        L2o --> L1o["Recovery (post)<br/>cleanup"]
    end

    L1o --> RES["Response"]
```

### 1.4 Crash Recovery Mechanism (setjmp/longjmp)

Lightweight exception handling using C's `setjmp`/`longjmp`:

```mermaid
sequenceDiagram
    participant REC as Recovery MW
    participant MW as Other Middleware
    participant H as Business Handler
    participant JB as jmp_buf (in ctx)

    REC->>JB: setjmp(ctx->jump_buffer) → 0
    Note over REC: First pass: proceed normally
    REC->>MW: csilk_next(ctx)
    MW->>H: csilk_next(ctx)

    alt Normal execution
        H->>H: csilk_string(ctx, 200, "OK")
        H-->>REC: Return through stack
    else Panic case
        H->>H: csilk_panic(ctx)
        Note over H: Trigger longjmp!
        H-->>JB: longjmp(ctx->jump_buffer, 1)
        JB-->>REC: setjmp returns 1
        REC->>REC: ctx->response.status = 500
        REC->>REC: csilk_string(ctx, 500, "Internal Server Error")
        REC->>REC: csilk_abort(ctx)
    end

    Note over REC: Response sent, server continues running
```

This ensures a single request crash never takes down the entire server process.

## 2. Memory Management Strategy

### 2.1 Per-Connection Arena Allocator

```mermaid
flowchart TB
    subgraph "Connection Lifecycle"
        C1["Connect"] --> A1["csilk_arena_new(4096)\nAllocates 4KB chunk"]
        A1 --> R1["Request 1\nAll allocations via arena bump"]
        R1 --> RESET1["csilk_arena_reset()\nO(1): chunk->used = 0"]
        RESET1 --> R2["Request 2\nSame 4KB chunk reused"]
        R2 --> RESET2["csilk_arena_reset()"]
        RESET2 --> R3["Request 3"]
        R3 --> CLOSE["Connection Close"]
        CLOSE --> FREE["csilk_arena_free()\nFree all chunks"]
    end

    subgraph "Arena Internal"
        CHUNKS["chunk1 (4KB) → chunk2 (4KB) → chunk3 (4KB)"]
        BUMP["Allocation: chunk->data + chunk->used\nchunk->used += size\n(just pointer arithmetic!)"]
    end
```

**Advantages**:
- Memory allocation is just **pointer offset** (extremely fast, no malloc/free per allocation)
- O(1) reset between requests (just set `used = 0`)
- Zero memory leaks: entire arena freed at connection close
- No heap fragmentation from per-request small allocations

## 3. Routing Engine: Radix Tree

### 3.1 Tree Structure

```mermaid
graph TB
    ROOT["/ (STATIC)"]

    ROOT --> A["api (STATIC)"]
    ROOT --> U["users (STATIC)"]
    ROOT --> S["static (STATIC)"]

    A --> V1["v1 (STATIC)"]
    A --> V2["v2 (STATIC)"]

    V1 --> V1U["users (STATIC)"]
    V2 --> V2U["users (STATIC)"]

    V1U --> GH1["GET: [list_v1_users, NULL]"]
    V2U --> GH2["GET: [list_v2_users, NULL]"]

    U --> PID[":id (PARAM)"]
    PID --> PRO["profile (STATIC)"]
    PID --> PO["posts (STATIC)"]
    PRO --> GH3["GET: [get_profile, NULL]"]
    PO --> GH4["GET: [get_posts, NULL]"]

    S --> WF["*filepath (WILDCARD)"]
    WF --> GH5["GET: [serve_static, NULL]"]
```

### 3.2 Matching Complexity

- **O(K)** where K = path length (number of segments)
- Fixed-size children array (`CSILK_MAX_CHILDREN = 128`) for CPU cache locality
- Backtracking for parameter nodes if recursive match fails

## 4. WebSocket Support

### 4.1 Handshake & Upgrade Flow

```mermaid
sequenceDiagram
    participant Client
    participant Server
    participant HTTP as llhttp
    participant WS as WebSocket Engine
    participant EvLoop as libuv

    Client->>Server: HTTP GET /ws (WebSocket upgrade)

    Note over Server,HTTP: Standard HTTP request parsed by llhttp
    Server->>Server: Router matches /ws handler

    Server->>WS: csilk_ws_handshake(ctx)
    WS->>WS: SHA1("abc123" + WS_GUID)
    WS->>WS: Base64(sha1_digest) → accept_key
    WS->>WS: ctx->is_websocket = 1

    Server->>Client: HTTP/1.1 101 Switching Protocols (WS upgrade accepted)

    Note over Server: Connection stays open, parser switches mode
    Note over EvLoop: on_read() now routes to csilk_ws_parse_frame()

    Client->>Server: WS Frame (text: "hello")
    Server->>WS: csilk_ws_parse_frame(ctx, buf, len)
    WS->>WS: Parse FIN + Opcode + Mask + Payload
    WS->>WS: Unmask payload with XOR mask key
    WS->>Client: ctx->on_ws_message(ctx, payload, len, opcode)

    Client->>Server: WS Close Frame (opcode 0x08)
    Server->>WS: Auto-respond with close frame
    Server->>EvLoop: uv_close() connection
```

### 4.2 Protocol Specification

- **Handshake**: RFC 6455 compliant SHA1 + Base64 key exchange
- **Frame Parsing**: Supports text (opcode 0x1) and binary (opcode 0x2) frames
- **Close Handshake**: Auto-detects close frames and responds per RFC 6455 Section 5.5.1
- **Async API**: `csilk_ws_send()` writes frames asynchronously via `uv_write()`

### 4.3 Internal Event Bus (Message Queue)

Csilk provides a built-in, asynchronous, topic-based Message Queue (`csilk_mq`) that facilitates communication between different parts of the application or bridging external events into the main loop.

```mermaid
graph LR
    WT["Worker Thread /<br/>External Event"] -- csilk_mq_publish --> ASYNC["uv_async_t<br/>(Signaling)"]
    ASYNC -- Loop Awake --> DISPATCH["MQ Dispatcher<br/>(Main Loop)"]
    DISPATCH --> GMW["Global MQ Middleware"]
    GMW --> TMW["Topic MQ Middleware"]
    TMW --> SUB["Subscribers"]
```

- **Thread-Safety**: `csilk_mq_publish` is safe to call from any thread. It uses `uv_async_send` to notify the main event loop.
- **Onion Model for Events**: The MQ system implements the same middleware pattern as the HTTP router. You can use `csilk_mq_use` for cross-cutting concerns (logging, validation) and `csilk_mq_subscribe` for final processing.
- **Low Overhead**: Messages are queued internally and processed in batches when the event loop wakes up, minimizing system call overhead.

## 5. Pluggable Database Drivers

csilk supports multiple database backends through a unified driver interface (`csilk/drivers/db.h`):

| Driver | Source | Protocol |
|--------|--------|----------|
| **SQLite** | `src/drivers/sqlite.c` | Local file (via sqlite3) |
| **MySQL** | `src/drivers/mysql.c` | TCP socket (via libmysqlclient) |
| **PostgreSQL** | `src/drivers/postgres.c` | TCP socket (via libpq) |
| **MongoDB** | `src/drivers/mongodb.c` | TCP socket (via libmongoc) |

The DB abstraction layer (`src/data/db.c`) manages connection pooling and provides a uniform API for queries regardless of backend.

## 6. Observability & Admin Dashboard

### 6.1 Prometheus Metrics

csilk includes a native metrics module for real-time monitoring of server health and performance.

**Metrics Collected:**

- **`http_requests_total`**: Counter of all processed HTTP requests, partitioned by `method`, `path`, and `status`.
- **`http_request_duration_seconds`**: Histogram of request latencies, useful for calculating P99/P95 response times.
- **`http_active_connections`**: Gauge of currently open TCP connections.

**Exposition Format:**

Metrics are exposed via the `/metrics` endpoint (using `csilk_metrics_handler`) in the standard Prometheus text-based format.

### 6.2 Unified Admin Dashboard

The admin module (`src/app/admin.c`) provides a real-time web dashboard:

```mermaid
graph TB
    subgraph "Admin Dashboard (/admin)"
        UI["admin_ui.html<br/>Single-page Application"]
        STATS["GET /admin/stats<br/>JSON metrics snapshot"]
        WS["GET /admin/ws<br/>WebSocket live events"]
    end

    subgraph "Data Sources"
        HTTP_M["HTTP Metrics<br/>(requests, latency)"]
        WF_M["Workflow Metrics<br/>(executions, tokens)"]
        MQ_M["MQ Metrics<br/>(messages, queues)"]
    end

    UI --> STATS
    UI --> WS
    STATS --> HTTP_M
    STATS --> WF_M
    STATS --> MQ_M
    WS --> HTTP_M
    WS --> WF_M
    WS --> MQ_M
```

- **HTTP Dashboard**: Real-time QPS, latency histogram, status code distribution, active connections.
- **Workflow Dashboard**: Live execution graph, node-level timing, token budget tracking.
- **MQ Dashboard**: Queue depth, message throughput, consumer lag.

### 6.3 Multi-Spectrum Telemetry

```mermaid
graph LR
    subgraph "Data Collection"
        HTTP["HTTP Metrics<br/>(atomic counters)"]
        WORKFLOW["Workflow Tracing<br/>(nanosecond precision)"]
        MQ["MQ Monitoring<br/>(queue depth)"]
        DB["DB Telemetry<br/>(pool status)"]
        AI["AI Telemetry<br/>(model calls)"]
        SYS["Process Metrics<br/>(RSS, CPU)"]
    end

    subgraph "Exposition"
        PROM["/metrics (Prometheus)"]
        ADMIN["/admin (Dashboard UI)"]
        ADMIN_STATS["/admin/stats (JSON)"]
        ADMIN_WS["/admin/ws (WebSocket)"]
    end

    HTTP --> PROM
    HTTP --> ADMIN
    WORKFLOW --> ADMIN
    MQ --> ADMIN
    DB --> ADMIN_STATS
    AI --> ADMIN_STATS
    SYS --> ADMIN_STATS
```

## 7. Document Generation

csilk uses **Doxygen** for API documentation:
- All public header files (`include/`) include complete `@brief`, `@param`, `@return` annotations
- All implementation files (`src/core/`, `src/app/`, `src/middleware/`, `src/drivers/`) include `@file`, `@brief`, `@copyright` and full `@param`/`@return` documentation
- Internal header `ctx_types.h` (formerly `context_internal.h`) in `include/csilk/core/` includes complete struct field documentation
- Example code (`examples/`) also includes Doxygen annotations
- Documentation generation command: `make docs` (requires Doxygen 1.12+)
- CI is configured for GitHub Pages auto-deployment of generated HTML documentation

## 8. Developer Guide

### 8.1 Writing WebSocket Handlers
```c
void ws_on_message(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode) {
    csilk_ws_send(c, (uint8_t*)"Hello Client", 12, 1);
}

void ws_handler(csilk_ctx_t* c) {
    csilk_ws_handshake(c);
    if (c->is_websocket) {
        c->on_ws_message = ws_on_message;
    }
}
```

### 8.2 Writing Middleware
```c
void my_middleware(csilk_ctx_t* c) {
    // Pre-logic: e.g., check Token
    csilk_next(c);
    // Post-logic: e.g., log latency
}
```

### 8.3 Starting the Server
```c
int main() {
    csilk_router_t* r = csilk_router_new();
    csilk_group_t* g = csilk_group_new(r, "/api");
    csilk_GET(g, "/ping", handler);

    csilk_server_t* s = csilk_server_new(r);
    csilk_server_run(s, 8080);
    return 0;
}
```

## 9. Multi-Worker Architecture

```mermaid
flowchart TB
    subgraph "Main Thread"
        ML["libuv Event Loop"]
        MT["TCP Socket (SO_REUSEPORT)"]
        MW["Global Middlewares\n(recovery, logger, ...)"]
    end

    subgraph "Worker 1"
        W1L["libuv Event Loop"]
        W1T["TCP Socket (SO_REUSEPORT)"]
        W1M["Global Middlewares"]
    end

    subgraph "Worker 2"
        W2L["libuv Event Loop"]
        W2T["TCP Socket (SO_REUSEPORT)"]
        W2M["Global Middlewares"]
    end

    subgraph "Worker N"
        WNL["libuv Event Loop"]
        WNT["TCP Socket (SO_REUSEPORT)"]
        WNM["Global Middlewares"]
    end

    K["Kernel TCP Stack\n<code>SO_REUSEPORT</code> distributes connections"] --> MT
    K --> W1T
    K --> W2T
    K --> WNT
```

Each worker thread runs its own libuv event loop with the same port bound via `SO_REUSEPORT`. The kernel distributes incoming connections across worker threads, enabling multi-core utilization without explicit inter-thread synchronization.

### 9.1 Client Pool Thread Safety

The `on_new_connection` callback executes on whichever event loop accepted the
connection — potentially any worker thread. The server uses a **per-worker
lock-free connection object pool** — each worker thread manages its own
`csilk_client_t` freelist, eliminating locking overhead:

```
pool_get: pop from per-worker freelist or calloc
pool_put: memset(0) → push to per-worker freelist or free
```

Without this mutex, two worker threads could retrieve the same client object,
then each call `uv_tcp_init` on their own event loop, causing `uv_accept` to
fail with: `Assertion 'server->loop == client->loop'`.

## 10. Performance Features

### 10.1 Zero-copy Static File Serving

For static files, Csilk implements a zero-copy mechanism using the `sendfile` system call (abstracted via `uv_fs_sendfile`).

- **Mechanism**: When a static file is requested, the server opens the file and stores the file descriptor in the context. Instead of reading the file into a buffer and writing it to the socket, the server calls `uv_fs_sendfile`, which directs the kernel to transfer data directly from the file system cache to the network socket.
- **Benefits**: Reduces CPU usage and memory bandwidth by eliminating data copying between kernel space and user space.

### 10.2 Request Tracing (Request ID)

The `csilk_request_id_middleware` ensures every request is assigned a unique UUID v4.

- **Propagation**: The ID is injected into the "X-Request-Id" response header and the thread-local logger state.
- **Correlation**: This allows all log entries generated during the processing of a single request to be correlated, even in a highly concurrent environment.

## 11. Final Directory Tree (Post-Restructuring)

```
csilk/
├── include/
│   ├── csilk.h                    # Umbrella (includes all module headers)
│   └── csilk/
│       ├── types.h                # Core types, constants, driver vtables
│       ├── context.h              # Request context accessor API
│       ├── response.h             # Response writing (status/JSON/redirect/chunked)
│       ├── router.h               # Radix-tree router
│       ├── server.h               # Server lifecycle + config
│       ├── middleware.h            # Built-in middleware function declarations
│       ├── websocket.h            # WebSocket API
│       ├── sse.h                  # Server-Sent Events API
│       ├── mq.h                   # Message Queue pub/sub API
│       ├── group.h                # Route groups
│       ├── hooks.h                # Lifecycle hook system
│       ├── workflow.h             # Workflow engine umbrella
│       ├── admin.h                # Admin dashboard umbrella
│       ├── errors.h               # HTTP status code macros
│       ├── config.h               # Server/app configuration structs
│       ├── version.h              # CSILK_VERSION macro
│       ├── app/
│       │   ├── app.h              # High-level app API
│       │   ├── workflow.h         # Workflow engine public API
│       │   └── workflow_wal.h     # WAL persistence API
│       ├── core/
│       │   ├── internal.h         # Internal umbrella → hash/codec/ws_frame/crypto_dispatch/mq_types
│       │   ├── hash.h             # SHA-1, SHA-256, HMAC-SHA256
│       │   ├── codec.h            # Base64, Base64URL, URL decode
│       │   ├── ws_frame.h         # WebSocket frame parsing
│       │   ├── crypto_dispatch.h  # Crypto/cipher dispatch stubs
│       │   ├── mq_types.h         # Internal MQ data structures
│       │   ├── ctx_types.h        # csilk_ctx_s struct layout
│       │   └── srv_types.h        # csilk_server_s / csilk_client_s layouts
│       ├── drivers/
│       │   ├── ai.h               # AI driver interface
│       │   ├── db.h               # DB driver interface
│       │   ├── cipher.h           # Cipher driver interface
│       │   └── perm.h             # Permission driver interface
│       ├── test/
│       │   └── test.h             # Test utilities (OOM simulation)
│       └── reflection/
│           └── reflect.h          # Runtime type reflection
├── src/
│   ├── core/                      # Server engine
│   │   ├── server.c               # Lifecycle: create/run/stop/free/hooks/workers
│   │   ├── connection.c           # Pool, accept, I/O, timers, on_read
│   │   ├── http1.c                # llHTTP callbacks, dispatch, response serialization
│   │   ├── tls.c                  # OpenSSL init, BIO-pair, ALPN negotiation
│   │   ├── context.c              # Request reading, lifecycle, binding, cookies
│   │   ├── response.c             # Response writing (status/JSON/redirect/chunked)
│   │   ├── router.c               # Radix-tree route matching
│   │   ├── arena.c                # Bump allocator
│   │   ├── config.c               # YAML configuration loader
│   │   ├── h2.c                   # HTTP/2 integration (nghttp2)
│   │   ├── h2.h                   # HTTP/2 internal header
│   │   ├── logger.c               # Structured logging
│   │   ├── recovery.c             # setjmp/longjmp error recovery
│   │   ├── url.c                  # URL parsing and splitting
│   │   ├── utils.c                # Encoding, crypto primitives
│   │   ├── test_utils.c           # Test OOM utilities
│   │   ├── admin.c                # Admin dashboard (moved from src/app/)
│   │   └── srv_impl.h             # Cross-file declarations for server split
│   ├── app/                       # Thin app wrappers (2 files)
│   │   ├── app.c
│   │   └── group.c
│   ├── workflow/                  # AI workflow engine (3 files)
│   │   ├── workflow.c
│   │   ├── workflow_loader.c
│   │   └── workflow_wal.c
│   ├── middleware/                 # 14 built-in middleware modules
│   ├── protocols/                 # WebSocket, Swagger
│   ├── drivers/                   # Organized by interface
│   │   ├── ai/                    # ollama.c, openai.c
│   │   ├── cipher/                # openssl.c
│   │   ├── perm/                  # simple.c
│   │   └── sqlite.c               # SQLite (flat, single file)
│   ├── ai/                        # AI unified interface
│   ├── data/                      # Database abstraction
│   ├── messaging/                 # Message Queue implementation
│   ├── reflection/                # Runtime type reflection
│   └── security/                  # Permission system
├── tests/                         # 109 unit/integration tests
├── examples/                      # Example applications
├── docs/                          # Documentation
├── cmake/                         # CMake modules
│   ├── sources.cmake              # Source file organization
│   └── tests.cmake                # Test registration
└── CMakeLists.txt                 # C23, version 0.3.0
```
