# Architecture

csilk follows a **layered event-driven architecture** with an **onion middleware model**, inspired by Go's Gin framework.

## Layer Architecture

```mermaid
graph TB
    subgraph "Layer 1: Application"
        H["User Handlers & Business Logic"]
        APP["csilk_app_t (Convenience Wrapper)"]
    end

    subgraph "Layer 2: Middleware"
        MW["Recovery | Logger | CORS | Auth | JWT\nRateLimit | CSRF | Static | Gzip | SSE | Multipart\nMetrics | RequestID | Validate | Session"]
    end

    subgraph "Layer 3: Core"
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

## Core Design Principles

### 1. Reactor Event-Driven Model with Native TLS

csilk uses libuv's event loop as its execution core. All I/O is non-blocking. Since v0.4.0, native TLS support is integrated via OpenSSL BIOs:

- **Encrypted read** -> `on_read` -> `BIO_write` -> `SSL_read` -> `llhttp_execute`
- **Encrypted write** -> `SSL_write` -> `BIO_read` -> `uv_write`

### 2. Onion Middleware & Hook System

#### Onion Middleware Model

Middleware forms a concentric "onion" where each layer wraps the next:

```mermaid
flowchart LR
    A["Client Request"] --> B

    subgraph Onion["Middleware Chain"]
        direction LR
        B["Logger (pre)"] --> C["Recovery (setjmp)"]
        C --> D["Auth (pre)"]
        D --> E["CORS (pre)"]
        E --> F["Handler\n(Business Logic)"]
        F --> G["CORS (post)"]
        G --> H["Auth (post)"]
        H --> I["Logger (post)"]
    end

    I --> J["HTTP Response"]
```

#### Hook System

In addition to the Onion model, a **Hook System** allows listening to global events without intercepting the request flow:
- `CSILK_HOOK_SERVER_START / STOP`
- `CSILK_HOOK_CONN_OPEN / CLOSE`
- `CSILK_HOOK_REQUEST_BEGIN / END`

### 3. Opaque Context & ABI Stability

Starting from v0.3.0, `csilk_ctx_t` is an **opaque type**. The internal structure is hidden in `include/csilk/core/context_internal.h`, ensuring that changes to the core engine do not break binary compatibility for third-party middleware and applications.

### 4. Pluggable Drivers

The framework now supports pluggable drivers for core services:
- **Storage Driver**: Customize how `csilk_set/get` values are stored (e.g., Redis for distributed sessions).
- **Crypto Driver**: Replace default hashing (SHA256, HMAC) and UUID generation with custom or hardware-accelerated implementations.

### 5. Arena Memory Management

Per-connection Arena Allocator eliminates malloc/free overhead:

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

### 6. Radix Tree Routing

Prefix tree routing with O(path_length) matching:

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

### 7. Multi-Worker SO_REUSEPORT

For multi-core utilization, csilk supports worker threads each with their own event loop:

```mermaid
flowchart TB
    subgraph "Main Thread"
        ML["libuv Event Loop"]
        MT["Main TCP Listener\n(SO_REUSEPORT)"]
    end

    subgraph "Worker Thread 1"
        W1L["libuv Event Loop"]
        W1T["TCP Listener\n(SO_REUSEPORT)"]
    end

    subgraph "Worker Thread 2"
        W2L["libuv Event Loop"]
        W2T["TCP Listener\n(SO_REUSEPORT)"]
    end

    subgraph "Worker Thread N"
        WNL["libuv Event Loop"]
        WNT["TCP Listener\n(SO_REUSEPORT)"]
    end

    K["Kernel\n(Connection Distribution)"] --> MT
    K --> W1T
    K --> W2T
    K --> WNT
```

## Request Lifecycle

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

## Key Data Structures

### csilk_ctx_t (Opaque Pointer)
The internal structure (hidden from public API) includes:
```c
struct csilk_ctx_s {
  int handler_index;              // Current position in handler chain
  csilk_handler_t* handlers;      // NULL-terminated handler array
  csilk_arena_t* arena;           // Per-connection arena allocator
  csilk_request_t request;        // Incoming request data
  csilk_response_t response;      // Outgoing response data
  char request_id[37];            // Unique Trace ID (UUID v4)
  csilk_storage_driver_t* storage_driver; // Custom storage backend
  csilk_crypto_driver_t* crypto_driver;   // Custom crypto backend (hash/HMAC/UUID)
  csilk_cipher_driver_t* cipher_driver;   // Custom cipher backend (AES/RSA/sign)
};
```

### csilk_server_s (server instance)
```c
struct csilk_server_s {
  uv_loop_t* loop;                // libuv event loop
  csilk_server_config_t config;   // Server configuration (including TLS)
  SSL_CTX* ssl_ctx;               // OpenSSL context
  csilk_hook_node_t* hooks[HOOK_COUNT]; // Registered event listeners
  atomic_int active_connections;  // Thread-safe connection count
  csilk_mq_t* mq;                 // Internal event bus
};
```

## Component Dependency Map

```mermaid
graph TB
    csilk.h["csilk.h<br/>(Public API)"] --> csilk/core/internal.h["csilk/core/internal.h<br/>(Internal API)"]
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
