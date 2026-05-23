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
        MW["Recovery | Logger | CORS | Auth\nRateLimit | CSRF | Static | Gzip | SSE | Multipart"]
    end

    subgraph "Layer 3: Core"
        subgraph "Request Processing"
            CTX["Context (csilk_ctx_t)"]
            ARENA["Arena Allocator"]
        end
        subgraph "Routing"
            RTR["Router (Radix Tree)"]
            GRP["Group (Hierarchical)"]
        end
        subgraph "Network & Protocol"
            SRV["Server (libuv TCP)"]
            HTTP["llhttp Parser"]
            WS["WebSocket Engine"]
        end
    end

    subgraph "Layer 4: Infrastructure"
        UV["libuv\n(Event Loop + Thread Pool)"]
        CJ["cJSON"]
        YAML["libyaml"]
        ZLIB["zlib"]
    end

    H --> CTX
    APP --> SRV
    MW --> CTX
    SRV --> RTR
    SRV --> CTX
    SRV --> HTTP
    SRV --> WS
    GRP --> RTR
    CTX --> ARENA
    SRV --> UV
    HTTP --> UV
    WS --> UV
    SRV --> CJ
    SRV --> YAML
    SRV --> ZLIB
```

## Core Design Principles

### 1. Reactor Event-Driven Model

csilk uses libuv's event loop as its execution core. All I/O is non-blocking:

- **Client connections** -> `on_new_connection` callback
- **Data reception** -> `on_read` callback
- **HTTP parsing** -> llhttp callbacks (6 state-machine callbacks)
- **Response writes** -> `on_write` callback
- **Idle timeouts** -> `on_timeout` callback

### 2. Onion Middleware Model

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

### 3. Arena Memory Management

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

### 4. Radix Tree Routing

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

### 5. Multi-Worker SO_REUSEPORT

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
    participant HTTP as llhttp Parser
    participant Server as csilk_server_t
    participant Router as Radix Tree Router
    participant MW as Middleware Chain
    participant Arena as Arena Allocator
    participant Response

    Client->>UV: TCP SYN
    UV->>Server: on_new_connection()
    Server->>Arena: csilk_arena_new(4096)
    Server->>UV: uv_read_start()
    UV->>Server: on_read(buf)

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
    end

    Server->>Arena: csilk_arena_reset()
    Note over Server: Keep-Alive: wait for next request<br>Non-Keep-Alive: close connection
```

## Key Data Structures

### csilk_ctx_t (per-request context)
```c
struct csilk_ctx_s {
  int handler_index;              // Current position in handler chain
  csilk_handler_t* handlers;      // NULL-terminated handler array
  int aborted;                    // Chain execution aborted
  jmp_buf jump_buffer;            // For panic recovery (setjmp/longjmp)
  csilk_arena_t* arena;           // Per-connection arena allocator
  csilk_request_t request;        // Incoming request data
  csilk_response_t response;      // Outgoing response data
  csilk_param_t params[20];       // URL path parameters
  int is_websocket;               // WebSocket upgrade flag
  int is_sse;                     // SSE initialization flag
  void (*on_ws_message)(...);     // WebSocket message callback
};
```

### csilk_server_s (server instance)
```c
struct csilk_server_s {
  uv_loop_t* loop;                // libuv event loop
  csilk_router_t* router;         // Route table
  uv_tcp_t server_handle;         // TCP server
  llhttp_settings_t settings;     // HTTP parser callbacks
  csilk_server_config_t config;   // Server configuration
  csilk_handler_t middlewares[32]; // Global middleware chain (max 32)
  int middleware_count;
  int max_connections;            // Connection limit (0 = unlimited)
  int active_connections;         // Current connection count
};
```

## Component Dependency Map

```mermaid
graph TB
    csilk.h["csilk.h<br/>(Public API)"] --> csilk_internal.h["csilk_internal.h<br/>(Internal API)"]
    csilk_app.h["csilk_app.h<br/>(High-Level API)"]

    subgraph src/core/
        server.c["server.c<br/>TCP + HTTP + libuv"] --> router.c["router.c<br/>Radix Tree"]
        server.c --> context.c["context.c<br/>Req/Res + Handlers"]
        server.c --> websocket.c["websocket.c<br/>WS Handshake + Frames"]
        context.c --> arena.c["arena.c<br/>Memory Pool"]
        context.c --> url.c["url.c<br/>URL Parsing"]
        server.c --> config.c["config.c<br/>YAML Config"]
        server.c --> logger.c["logger.c<br/>Structured Logging"]
        server.c --> reflect.c["reflect.c<br/>JSON <-> C Struct"]
        utils.c["utils.c<br/>SHA1 + Base64"]
    end

    subgraph src/middleware/
        recovery.c --> context.c
        logger_mw.c["logger.c"] --> context.c
        auth.c --> context.c
        cors.c --> context.c
        ratelimit.c --> context.c
        csrf.c --> context.c
        static_mw.c["static.c"] --> context.c
        gzip.c --> context.c
        sse.c --> context.c
        multipart.c --> context.c
    end

    subgraph src/app/
        app.c["app.c<br/>High-Level Wrapper"] --> server.c
        app.c --> router.c
        app.c --> config.c
    end

    server.c --> libuv[libuv]
    server.c --> llhttp[llhttp]
    context.c --> cjson[cJSON]
    config.c --> libyaml[libyaml]
    gzip.c --> zlib[zlib]
```
