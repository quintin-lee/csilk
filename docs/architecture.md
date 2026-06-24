# csilk Architecture Whitepaper & Technical Manual

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-24

csilk is a lightweight, high-performance HTTP web framework written in C, adopting a **layered event-driven architecture** combined with an **onion middleware model**, inspired by Go's Gin framework and powered by libuv, llhttp, nghttp2, and cJSON.

---

## 1. Layer Architecture

```mermaid
graph TB
    subgraph "Layer 1: Application"
        H["User Handlers & Business Logic"]
        APP["csilk_app_t (Convenience Wrapper)"]
    end

    subgraph "Layer 2: Middleware"
        MW["Recovery | Logger | CORS | Auth | JWT | WAF<br/>RateLimit | CSRF | Static | Gzip | SSE | Multipart<br/>Metrics | RequestID | Validate | Session"]
    end

    subgraph "Layer 3: Core Engine"
        subgraph "Request Processing"
            CTX["Context (csilk_ctx_t)"]
            ARENA["Arena Allocator"]
            HOOKS["Hook System"]
        end
        subgraph "AI & Data"
            AI["AI Unified Engine"]
            DB["DB Abstraction (SQLite/MySQL/PG/MongoDB)"]
            REFL["Reflection Engine"]
        end
        subgraph "Routing"
            RTR["Router (Radix Tree)"]
            GRP["Group (Hierarchical)"]
        end
        subgraph "Network & Protocol"
            SRV["Server (libuv TCP)"]
            HTTP["llhttp Parser"]
            TLS["OpenSSL (TLS/SSL)"]
            WS["WebSocket Engine"]
        end
    end

    subgraph "Layer 4: Infrastructure"
        UV["libuv\n(Event Loop + Thread Pool)"]
        CJ["cJSON"]
        YAML["libyaml"]
        ZLIB["zlib"]
        SSL["OpenSSL"]
        CURL["libcurl"]
        MQ["csilk_mq_t\n(Message Queue)"]
    end

    subgraph "Observability"
        METRICS["Prometheus /metrics"]
        ADMIN["Admin Dashboard /admin"]
        ADMIN_STATS["Admin Stats /admin/stats"]
        ADMIN_WS["Admin WS /admin/ws"]
    end

    H --> CTX
    APP --> SRV
    MW --> CTX
    SRV --> RTR
    SRV --> CTX
    SRV --> HTTP
    SRV --> TLS
    SRV --> WS
    AI --> CURL
    AI --> CJ
    GRP --> RTR
    CTX --> ARENA
    SRV --> UV
    HTTP --> UV
    TLS --> SSL
    WS --> UV
    SRV --> CJ
    SRV --> YAML
    SRV --> ZLIB
    SRV --> MQ
    ADMIN --> METRICS
    ADMIN --> SRV
    ADMIN --> MQ
```

---

## 2. Core Design Principles

### 2.1 Reactor Event-Driven Model with Native TLS & ALPN
The framework is built on `libuv`, ensuring all network I/O is non-blocking. 

* **Protocol Dispatcher**: During TLS ALPN negotiation, a dispatcher routes decrypted traffic to either `llhttp` (HTTP/1.1) or `nghttp2` (HTTP/2) based on ALPN (`h2` vs `http/1.1`).
* **HTTP/1.1 parsing**: `llhttp` drives a state-machine parser to process HTTP/1.1 requests. During parsing callbacks, `csilk` uses **Zero-copy HTTP parsing** using string views (`csilk_str_view_t`) that directly reference raw network receive buffers, completely eliminating dynamic `malloc`/`realloc`/`free` heap allocations for HTTP headers, URLs, and bodies.
* **HTTP/2 parsing**: `nghttp2` processes binary HTTP/2 frames, HPACK headers, and handles multiplexed streams.
* **Native TLS integration**: OpenSSL BIO-pairs handle encrypted network traffic directly on the event loop:
  - **Encrypted read** -> `on_read` -> `BIO_write` -> `SSL_read` -> `llhttp_execute` / `csilk_h2_process_data`
  - **Encrypted write** -> `SSL_write` -> `BIO_read` -> `uv_write`

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

### 2.2 Onion Middleware Model
Middleware implements bidirectional request interception through the `csilk_next()` mechanism.

```mermaid
flowchart LR
    REQ["Request"]

    subgraph "Onion Layers"
        direction LR
        L1i["Recovery (pre)<br/>setjmp()"] --> L2i["Logger (pre)<br/>start timer"]
        L2i --> L3i["WAF (pre)<br/>check rules"]
        L3i --> L4i["Auth (pre)<br/>check token"]
        L4i --> L5i["RateLimit (pre)<br/>check quota"]
        L5i --> CORE["Business Handler<br/>(csilk_string/json)"]
        CORE --> L5o["RateLimit (post)<br/>(no-op)"]
        L5o --> L4o["Auth (post)<br/>(no-op)"]
        L4o --> L3o["WAF (post)<br/>(no-op)"]
        L3o --> L2o["Logger (post)<br/>log latency"]
        L2o --> L1o["Recovery (post)<br/>cleanup"]
    end

    L1o --> RES["Response"]
```

### 2.3 Hook System
In addition to onion middleware, a hook system allows non-blocking observation of global lifecycle events:
* `CSILK_HOOK_SERVER_START` / `CSILK_HOOK_SERVER_STOP`
* `CSILK_HOOK_CONN_OPEN` / `CSILK_HOOK_CONN_CLOSE`
* `CSILK_HOOK_REQUEST_BEGIN` / `CSILK_HOOK_REQUEST_END`

### 2.4 Opaque Context & ABI Stability
Starting from v0.3.0, `csilk_ctx_t` is defined as an **opaque pointer**. The internal structure layout is hidden in `include/csilk/core/ctx_types.h`. This guarantees binary compatibility (ABI stability) for third-party middleware and user applications when the core engine structure undergoes internal modifications.

### 2.5 Pluggable Drivers
Services are pluggable through clean interfaces:
* **Storage Driver**: Backing store for `csilk_set/get` session variables (e.g. SQLite, Redis).
* **Crypto/Cipher Driver**: Interchangeable cryptography backends (e.g. OpenSSL).
* **AI Driver**: Interface to generic LLM providers (e.g. OpenAI, Ollama).
* **Vector DB Driver**: Interface to vector databases (e.g. Qdrant, Milvus).

### 2.6 Per-Connection Arena Memory Management
An Arena Allocator maps blocks (4KB default) per-connection. All allocations during request processing (headers, JSON parsing, URL splits) use pointer bump allocation.

```mermaid
flowchart TB
    subgraph "Request 1"
        A1["Arena Block 1\n(4KB default)"]
        A1 --> P1["Allocate via pointer bump\nHeaders, Body, Strings"]
        P1 --> F1["Arena Reset (O(1))\nAfter response sent"]
    end

    subgraph "Request 2 (Keep-Alive)"
        A2["Arena Block 1 (Reused)\nMemory already mapped"]
        A2 --> P2["Allocate via pointer bump\nHeaders, Body, Strings"]
        P2 --> F2["Arena Reset (O(1))"]
    end

    subgraph "Connection Close"
        FREE["csilk_arena_free()\nRelease all chunks"]
    end

    A1 --> A2
    F2 --> FREE
```

**Key Advantages:**
* Allocation is reduced to simple pointer arithmetic (no call to malloc/free per request object).
* O(1) reset between requests (setting offset pointer `used = 0`).
* Avoids heap fragmentation. Entire memory pool is freed in one pass when the TCP connection closes.

### 2.7 Radix Tree Routing
Prefix tree routing with O(path_length) matching, support for static, parameterized, and wildcard routes.

```mermaid
graph TB
    ROOT["/ (root)"]

    subgraph "Static Routes"
        ROOT --> A["api"]
        A --> B["v1"]
        B --> C["users (GET)"]
        A --> D["v2"]
        D --> E["users (GET)"]
    end

    subgraph "Parameter Routes"
        ROOT --> F["users"]
        F --> G[":id (PARAM)"]
        G --> H["profile (GET)"]
        G --> I["posts (GET)"]
    end

    subgraph "Wildcard Routes"
        ROOT --> J["static"]
        J --> K["*filepath (WILDCARD)"]
    end

    C -.- S1["/api/v1/users"]
    E -.- S2["/api/v2/users"]
    H -.- S3["/users/42/profile"]
    I -.- S4["/users/42/posts"]
    K -.- S5["/static/css/app.css"]
```

---

## 3. Crash Recovery Mechanism (setjmp/longjmp)

Lightweight exception handling routes panics cleanly back to the recovery middleware:

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

---

## 4. WebSocket & Message Queue

### 4.1 WebSocket Upgrade Flow

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
    Client->>Client: Send Frame
    Client->>Server: WS Frame (text: "hello")
    Server->>WS: csilk_ws_parse_frame(ctx, buf, len)
    WS->>WS: Parse FIN + Opcode + Mask + Payload
    WS->>WS: Unmask payload with XOR mask key
    WS->>Client: ctx->on_ws_message(ctx, payload, len, opcode)
    Client->>Server: WS Close Frame (opcode 0x08)
    Server->>WS: Auto-respond with close frame
    Server->>EvLoop: uv_close() connection
```

* **WebSocket Framing**: Full support for text (opcode 0x1), binary (opcode 0x2), and close frames (opcode 0x8) under RFC 6455.
* **Asynchronous delivery**: `csilk_ws_send` runs asynchronously via `uv_write`.

### 4.2 Thread-Safe Message Queue (csilk_mq)
csilk provides a built-in topic-based message queue system enabling thread-safe communication:

```mermaid
graph LR
    WT["Worker Thread /<br/>External Event"] -- csilk_mq_publish --> ASYNC["uv_async_t<br/>(Signaling)"]
    ASYNC -- Loop Awake --> DISPATCH["MQ Dispatcher<br/>(Main Loop)"]
    DISPATCH --> GMW["Global MQ Middleware"]
    GMW --> TMW["Topic MQ Middleware"]
    TMW --> SUB["Subscribers"]
```

* `csilk_mq_publish` is safe to call from any thread. It signals the main loop using `uv_async_send`.
* The MQ system implements the onion middleware pattern (`csilk_mq_use`) to process topics uniformly.

---

## 5. Multi-Worker Architecture

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

### Client Pool Thread Safety
Since connection requests run on whatever worker thread accepted them, a lock-free per-worker connection pool is implemented. Each worker loop manages its own `csilk_client_t` freelist, eliminating mutex contention and guaranteeing thread safety when workers construct client structures.

---

## 6. Request Lifecycle

The sequence diagram below shows the complete lifecycle of a request from client to network response:

```mermaid
sequenceDiagram
    participant Client
    participant UV as libuv Event Loop
    participant SSL as OpenSSL (TLS)
    participant HTTP as llhttp Parser
    participant Server as csilk_server_t
    participant Router as Radix Tree Router
    participant MW as Middleware Chain
    participant Arena as Arena Allocator
    participant Response

    Client->>UV: TCP SYN
    UV->>Server: on_new_connection()
    Server->>Arena: csilk_arena_new(4096)
    
    alt is_tls
        Server->>SSL: SSL_do_handshake()
    end

    Server->>UV: uv_read_start()
    UV->>Server: on_read(encrypted_buf)
    
    alt is_tls
        Server->>SSL: SSL_read()
    end
    
    loop HTTP Parsing
        Server->>HTTP: llhttp_execute()
        HTTP-->>Server: on_url()
        HTTP-->>Server: on_header_field() / on_header_value()
        HTTP-->>Server: on_headers_complete()
        HTTP-->>Server: on_body()
        HTTP-->>Server: on_message_complete()
    end

    Server->>Server: finalize_request()
    Server->>Router: csilk_router_match_ctx()
    Router-->>Server: handler chain found
    Server->>Server: Prepend global middlewares
    Server->>MW: csilk_next(ctx)

    loop Onion Chain
        MW->>MW: Middleware N (pre-logic)
        MW->>MW: csilk_next()
        MW->>MW: Handler (business logic)
        MW->>MW: csilk_next()
        MW->>MW: Middleware N (post-logic)
    end

    alt is_async
        MW-->>Server: return (response deferred)
    else synchronous
        MW->>Server: csilk_string/json is set
        Server->>Response: _csilk_send_response()
        alt is_tls
            Server->>SSL: SSL_write()
        end
    end

    Server->>Arena: csilk_arena_reset()
    Note over Server: Keep-Alive: wait for next request<br>Non-Keep-Alive: close connection
```

---

## 7. Database Driver Matrix

Unified database drivers implement a standard interface (`csilk/drivers/db.h`):

| Driver | Source File | Protocol/Dependency | Connection Model |
|--------|-------------|---------------------|------------------|
| **SQLite** | `src/drivers/sqlite.c` | Local File (sqlite3) | Local DB instance |
| **MySQL** | `src/drivers/mysql.c` | TCP Socket (libmysqlclient) | Thread pool connection pool |
| **PostgreSQL** | `src/drivers/postgres.c` | TCP Socket (libpq) | Thread pool connection pool |
| **MongoDB** | `src/drivers/mongodb.c` | TCP Socket (libmongoc) | Driver pool manager |

---

## 8. Observability & Dashboard

### 8.1 Prometheus Metrics
Native metrics exposition format:
* `http_requests_total`: Counter partitioned by method, path, and status code.
* `http_request_duration_seconds`: Histogram bucket for request latency calculations (P99, P95).
* `http_active_connections`: Current active connection counts.

### 8.2 Admin Dashboard (/admin)

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

---

## 9. Performance Features

* **Zero-copy Static File Serving**: Serves files using `sendfile` system calls (abstracted as `uv_fs_sendfile`). Transmit calls happen directly from the kernel cache to the socket descriptor, avoiding user-space context switches.
* **Trace correlation**: Request ID middleware injects a unique UUID v4 into the request headers and logging telemetry.

---

## 10. Developer Guide

### 10.1 Writing WebSocket Handlers
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

### 10.2 Writing Middleware
```c
void my_middleware(csilk_ctx_t* c) {
    // Pre-logic: e.g., check Token
    csilk_next(c);
    // Post-logic: e.g., log latency
}
```

### 10.3 Starting the Server
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

---

## 11. Component Dependency Map

```mermaid
graph TB
    csilk.h["csilk.h<br/>(Public API)"] --> csilk/core/internal.h["csilk/core/internal.h<br/>(Internal API umbrella)"]
    csilk/core/internal.h["csilk/core/internal.h"] --> csilk/core/hash.h["hash.h<br/>(SHA-1/SHA-256/HMAC)"]
    csilk/core/internal.h --> csilk/core/codec.h["codec.h<br/>(Base64/URL-decode)"]
    csilk/core/internal.h --> csilk/core/ws_frame.h["ws_frame.h<br/>(WebSocket frames)"]
    csilk/core/internal.h --> csilk/core/crypto_dispatch.h["crypto_dispatch.h<br/>(Crypto stubs)"]
    csilk/core/internal.h --> csilk/core/mq_types.h["mq_types.h<br/>(Message Queue)"]
    csilk/app/app.h["csilk/app/app.h<br/>(High-Level API)"]
    csilk/app/admin.h["csilk/app/admin.h<br/>(Admin Dashboard API)"]

    subgraph src/core/
        server.c["server.c<br/>TCP + HTTP + libuv"] --> router.c["router.c<br/>Radix Tree"]
        server.c --> context.c["context.c<br/>Req/Res + Handlers"]
        server.c --> websocket.c["websocket.c<br/>WS Handshake + Frames"]
        context.c --> arena.c["arena.c<br/>Memory Pool"]
        context.c --> url.c["url.c<br/>URL Parsing"]
        server.c --> config.c["config.c<br/>YAML Config"]
        server.c --> logger.c["logger.c<br/>Structured Logging"]
        server.c --> reflect.c["reflect.c<br/>JSON <-> C Struct"]
        ai.c["ai.c<br/>Unified AI Engine"] --> ai_openai.c["ai_openai.c<br/>OpenAI Driver"]
        ai.c --> ai_ollama.c["ai_ollama.c<br/>Ollama Driver"]
        utils.c["utils.c<br/>SHA1 + Base64"]
    end

    subgraph src/middleware/ (15 modules)
        logger_mw.c["logger.c"] --> context.c
        auth.c --> context.c
        jwt.c["jwt.c<br/>JWT Auth"] --> context.c
        cors.c --> context.c
        ratelimit.c --> context.c
        csrf.c --> context.c
        static_mw.c["static.c"] --> context.c
        gzip.c --> context.c
        sse.c --> context.c
        multipart.c --> context.c
        metrics.c["metrics.c<br/>Prometheus"] --> context.c
        request_id.c["request_id.c"] --> context.c
        session.c["session.c"] --> context.c
        validate.c["validate.c"] --> context.c
    end

    subgraph src/app/
        app.c["app.c<br/>High-Level Wrapper"] --> server.c
        app.c --> router.c
        app.c --> config.c
        admin.c["admin.c<br/>Dashboard"] --> server.c
        admin.c --> mq.c["mq.c<br/>Message Queue"]
    end

    subgraph src/drivers/
        sqlite.c["sqlite.c"] --> data/db.c
        mysql.c["mysql.c"] --> data/db.c
        postgres.c["postgres.c"] --> data/db.c
        mongodb.c["mongodb.c"] --> data/db.c
    end

    server.c --> libuv[libuv]
    server.c --> llhttp[llhttp]
    context.c --> cjson[cJSON]
```

---

## 12. Directory Tree Structure

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
│       ├── errors.h               # HTTP status code constants
│       ├── config.h               # Server/app configuration structs
│       ├── crypto.h               # Cryptographic utility functions
│       ├── hot_reload.h           # Hot-reload API
│       ├── version.h              # CSILK_VERSION macro (generated)
│       ├── app/
│       │   ├── app.h              # High-level app API
│       │   ├── workflow.h         # Workflow engine public API
│       │   └── workflow_wal.h     # WAL persistence API
│       ├── core/
│       │   ├── internal.h         # Internal umbrella
│       │   ├── hash.h             # SHA-1, SHA-256, HMAC-SHA256
│       │   ├── codec.h            # Base64, Base64URL, URL decode
│       │   ├── bounded_buf.h      # Stack-only bounded string/JSON builder
│       │   ├── ws_frame.h         # WebSocket frame parsing
│       │   ├── crypto_dispatch.h  # Crypto/cipher dispatch stubs
│       │   ├── mq_types.h         # Internal MQ data structures
│       │   ├── ctx_types.h        # csilk_ctx_s struct layout
│       │   └── srv_types.h        # csilk_server_s / csilk_client_s layouts
│       ├── drivers/
│       │   ├── ai.h               # AI driver interface
│       │   ├── db.h               # DB driver interface
│       │   ├── cipher.h           # Cipher driver interface
│       │   ├── perm.h             # Permission driver interface
│       │   └── vector.h           # Vector DB driver interface
│       ├── test/
│       │   └── test.h             # Test utilities (OOM simulation, context helpers)
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
│   │   ├── utils.c                # misc utility functions
│   │   ├── base64.c               # Base64/Base64URL encode/decode
│   │   ├── sha1.c                 # SHA-1 hash (WebSocket handshake)
│   │   ├── uuid.c                 # UUID v4 generation
│   │   ├── bounded_buf.c          # Stack-only bounded string/JSON builder
│   │   ├── hot_reload.c           # File-watch hot-reload (inotify)
│   │   ├── test_utils.c           # Test OOM utilities
│   │   ├── admin.c                # Admin dashboard
│   │   ├── srv_impl.h             # Cross-file declarations for server split
│   │   └── srv_internal.h         # Internal server types
│   ├── app/                       # Thin app wrappers
│   │   ├── app.c
│   │   └── group.c
│   ├── data/                      # Database abstraction
│   │   ├── db.c                   # Pool lifecycle, query/exec dispatch
│   │   └── db_internal.h          # csilk_db_pool_s private struct
│   ├── ai/                        # AI unified interface
│   │   └── ai.c
│   ├── workflow/                  # AI workflow engine
│   │   ├── wf_ai.c                # AI chat nodes, memory helper, templates
│   │   ├── wf_lifecycle.c         # Lifecycle: creation, destruction, registration
│   │   ├── wf_monitor.c           # Real-time monitor events
│   │   ├── wf_scheduler.c         # Execution engine, DAG scheduling, WAL restore
│   │   ├── wf_trace.c             # Execution trace recording
│   │   ├── workflow_internal.h    # Internal structures & stubs
│   │   ├── workflow_loader.c      # JSON/YAML parser loaders
│   │   └── workflow_wal.c         # WAL persistence engine
│   ├── middleware/                # Built-in middleware modules
│   ├── protocols/                 # WebSocket, Swagger
│   ├── drivers/                   # Driver implementations
│   ├── messaging/                 # Message Queue
│   ├── reflection/                # Runtime type reflection
│   ├── security/                  # Permission system
│   └── util/                      # Utility modules
├── tests/                         # Unit/integration/fuzz tests
├── examples/                      # Example applications
├── docs/                          # Architecture, research, analysis docs
├── cmake/                         # CMake modules
└── CMakeLists.txt                 # C23, version 0.5.0-dev
```

---

## 13. Document Generation

csilk uses **Doxygen** to build API documentation from annotated files:
* All public headers in `include/` and implementation files in `src/` include full Doxygen tags (`@brief`, `@param`, `@return`).
* Command to generate documentation locally: `make docs` (requires Doxygen 1.12+).
* CI is configured for GitHub Pages auto-deployment of the generated HTML docs.
