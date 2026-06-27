# Advanced Usage

This guide covers advanced csilk usage patterns: WebSocket, SSE, multipart uploads, gzip compression, static file serving, and multi-worker configuration. Code examples **SHOULD** be adapted to your specific use case. All middleware **MUST** be registered before `csilk_server_run()`.

## Route Groups

Route groups allow prefix-based route organization and middleware scoping:

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
graph TB
    subgraph Router
        ROOT["/ "]
    end

    subgraph api_group["Group /api (auth middleware)"]
        API["/api"]
        API --> V1["/api/v1"]
        API --> V2["/api/v2"]
        V1 --> V1U["/api/v1/users"]
        V1 --> V1P["/api/v1/posts"]
        V2 --> V2U["/api/v2/users"]

        Note1["Middleware: auth_handler\nApplied to all /api routes"]
    end

    subgraph admin_group["Group /admin (auth + admin middleware)"]
        ADMIN["/admin"]
        ADMIN --> DASH["/admin/dashboard"]
        ADMIN --> SETTINGS["/admin/settings"]

        Note2["Middleware: auth_handler + admin_check\nInherits parent + adds own"]
    end

    ROOT --> API
    ROOT --> ADMIN
```

### Group Code Example

```c
csilk_group_t* api = csilk_group_new(router, "/api");
csilk_group_use(api, auth_handler);     // All /api/* routes require auth
csilk_GET(api, "/users", list_users);

csilk_group_t* admin = csilk_group_group(api, "/admin");
csilk_group_use(admin, admin_handler);  // /api/admin/* requires auth + admin
csilk_GET(admin, "/dashboard", dashboard);
```

## WebSocket Protocol

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant HTTP as fa:fa-file-code llhttp Parser
    participant CTX as fa:fa-cube csilk_ctx_t
    participant WS as fa:fa-plug WebSocket Engine

    Client->>Server: GET /ws (WebSocket upgrade request)

    Note over Server: Regular HTTP parsing

    Server->>CTX: Router matches /ws handler
    CTX->>CTX: ws_handler(ctx)

    CTX->>WS: csilk_ws_handshake(ctx)
    WS->>WS: SHA1(key + GUID) → base64 → accept_key
    WS->>CTX: Set headers: Upgrade + Connection + Accept
    WS->>CTX: ctx->status = 101 SWITCHING_PROTOCOLS
    WS->>CTX: ctx->is_websocket = 1

    CTX->>Server: _csilk_send_response() → 101 Switching Protocols
    Server-->>Client: HTTP/1.1 101 Switching Protocols

    Note over Server: uv_read_start continues but...
    Note over Server: on_read now routes to csilk_ws_parse_frame()

    Client->>Server: WebSocket frame (text: "Hello")
    Server->>WS: csilk_ws_parse_frame(ctx, buf, nread)
    WS->>WS: Unmask payload
    WS->>WS: Check opcode (0x08 = close, else data)
    WS->>CTX: ctx->on_ws_message(ctx, payload, len, opcode)

    CTX->>WS: csilk_ws_send(ctx, "Reply", 5, 1)
    WS->>WS: Build frame: FIN + Opcode + Length + Payload
    WS->>Server: uv_write() → client

    Client->>Server: Close frame (opcode 0x08)
    Server->>WS: Auto-respond with close frame
    Server->>Server: uv_close() connection
```

### WebSocket Frame Format

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
graph LR
    subgraph frame["fa:fa-th-large Frame Structure"]
        B0["Byte 0: FIN(1) + RSV(3) + Opcode(4)"]
        B1["Byte 1: MASK(1) + PayloadLen(7)"]
        EL["Extended Length: 0/2/8 bytes"]
        MK["Mask Key: 0 or 4 bytes"]
        PL["Payload Data"]
        B0 --> B1 --> EL --> MK --> PL
    end
```

## Server-Sent Events (SSE)

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant CTX as fa:fa-cube csilk_ctx_t
    participant SSE

    Client->>Server: GET /events
    Server->>CTX: sse_handler(ctx)
    CTX->>SSE: csilk_sse_init(ctx)
    SSE->>CTX: Set Content-Type: text/event-stream
    SSE->>CTX: Set Cache-Control: no-cache
    SSE->>CTX: Set Connection: keep-alive
    SSE->>CTX: ctx->is_sse = 1
    CTX->>Server: Send headers (chunked)

    Note over Server: Connection stays open

    loop Event Stream
        CTX->>SSE: csilk_sse_send(ctx, "message", "data here")
        SSE->>Server: write_chunk_frame (SSE event message)
        Server-->>Client: HTTP chunk

    end

    CTX->>SSE: csilk_sse_close(ctx)
    SSE->>Server: write_chunk_frame (terminal chunk)
    Server-->>Client: Terminal chunk
```

## Multipart Form Data

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant CTX as fa:fa-cube csilk_ctx_t
    participant MP as fa:fa-upload Multipart Middleware
    participant Handler as Part Callback

    Client->>Server: POST /upload (multipart, boundary=abc123)

    Server->>CTX: Router matches /upload
    CTX->>MP: csilk_multipart_parse(ctx, part_handler)
    MP->>MP: Extract boundary from Content-Type
    MP->>MP: Parse each part

    loop For each part
        MP->>MP: Find boundary separator
        MP->>MP: Parse Content-Disposition → name, filename
        MP->>MP: Parse Content-Type (if present)
        MP->>Handler: part_handler(ctx, part_name, filename, data, len, NULL)
    end

    MP->>MP: Parse closing boundary
```

## Gzip Compression

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant Context
    participant Gzip MW as fa:fa-archive Gzip Middleware
    participant TP as fa:fa-tasks libuv Thread Pool
    participant Zlib

    Client->>Server: GET /data (with Accept-Encoding: gzip)

    Note over Server: Middleware captures response

    Server->>Gzip MW: gzip_handler(ctx)
    Gzip MW->>Gzip MW: csilk_next(ctx)
    Note over Gzip MW: Wait for handler to set response body

    Gzip MW->>Gzip MW: Check Accept-Encoding header
    alt Client accepts gzip
        Gzip MW->>TP: Offload compression work
        TP->>Zlib: deflate(response.body)
        Zlib-->>TP: compressed data
        TP-->>Gzip MW: async callback
        Gzip MW->>Context: Set Content-Encoding: gzip
        Gzip MW->>Context: Replace response.body with compressed data
    end

    Gzip MW->>Server: _csilk_send_response()
    Server-->>Client: HTTP Response (compressed)
```

## Static File Serving

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant Context
    participant SMW as fa:fa-folder-open Static Middleware
    participant TP as fa:fa-tasks libuv Thread Pool
    participant FS as Filesystem

    Client->>Server: GET /static/css/app.css

    Server->>Context: Router matches /static/*filepath
    Context->>Context: param = "css/app.css"
    Context->>SMW: csilk_static(ctx, "./public")
    SMW->>SMW: Build full path: ./public/css/app.css
    SMW->>SMW: Check path traversal attack (..)

    SMW->>TP: Offload file read to thread pool
    TP->>FS: open() + fstat() + read()
    FS-->>TP: file contents + size
    TP-->>SMW: async callback

    SMW->>Context: Set Content-Type (MIME from extension)
    SMW->>Context: csilk_string(ctx, 200, file_contents)
    SMW->>Server: _csilk_send_response()
    Server-->>Client: HTTP Response (file contents)
```

## Admin Dashboard

The unified admin dashboard provides real-time monitoring of your csilk application:

```c
#include "csilk/app/admin.h"

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");

    // Register admin dashboard under /admin
    csilk_admin_serve(app, "/admin");

    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

The dashboard includes:
- **HTTP Metrics**: QPS, latency histogram, status code distribution, active connections.
- **Workflow Monitoring**: Live execution graph, node-level timing, token budget tracking.
- **MQ Monitoring**: Queue depth, message throughput, consumer lag.
- **Database Telemetry**: Connection pool status, query latency.
- **AI Telemetry**: Model call count, token usage, error rates.
- **Process Metrics**: RSS memory, CPU usage, uptime.

## Database Drivers

csilk supports four database backends through a unified driver interface:

```c
#include "csilk/drivers/db.h"

// Initialize database pool from config
csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "data/app.db", 10);

// Execute query
csilk_db_result_t* result = csilk_db_query(pool, "SELECT * FROM users WHERE id = ?", 1);
if (result && result->row_count > 0) {
    printf("User: %s\n", result->rows[0][1]); // column 1 = name
}
csilk_db_result_free(result);
csilk_db_pool_free(pool);
```

## High-Level App API

The `csilk_app_t` wrapper simplifies server creation:

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    actor User
    participant App as fa:fa-cube csilk_app_t
    participant Config
    participant Router
    participant Server

    User->>App: csilk_app_new("config.yaml")
    App->>Config: csilk_load_config("config.yaml")
    Config-->>App: csilk_config_t

    App->>Router: csilk_router_new()
    App->>Server: csilk_server_new(router)

    User->>App: csilk_app_use(app, cors_middleware)
    App->>Server: csilk_server_use(server, cors_middleware)

    User->>App: csilk_app_get(app, "/hello", handler)
    App->>Router: csilk_router_add(router, "GET", "/hello", handler)

    User->>App: csilk_app_apply_config(app)
    App->>App: Apply config.settings to server
    App->>App: Apply config.middlewares to server
    App->>App: Apply config.routes to router

    User->>App: csilk_app_run(app, 8080)
    App->>Server: csilk_server_set_config(server, config)
    App->>Server: csilk_server_run(server, 8080)
    Note over Server: Blocking event loop
```

## Multi-Worker Mode

When `worker_threads > 1`, csilk uses `SO_REUSEPORT` to bind multiple listener
sockets — one per worker thread plus the main thread. The kernel distributes
incoming connections across all listeners.

### Thread Safety

In multi-worker mode, connection callbacks (`on_new_connection`) can execute on
any event loop thread. All shared mutable state accessed during connection
establishment must be thread-safe:

- **Client connection pool** (`pool_get`/`pool_put`): Each worker thread manages its own lock-free connection object pool, avoiding mutex contention.
- **Active client list**: Protected by `clients_mutex`.
- **Connection counters**: Use atomic operations (`atomic_fetch_add`).

### Graceful Shutdown

`csilk_server_stop()` signals the main loop to close its listener and
connections, then signals each worker thread to do the same via per-worker
`uv_async_t` handles. The main thread joins all workers in
`csilk_server_free()` after the event loop exits.

---

## Further Reading

For deep-dive architectural details of features covered in this guide:

| Feature | Module Design Document |
|---------|----------------------|
| Route Groups & Router Internals | [Router](../module-design/router.md) |
| WebSocket Protocol | [Protocols](../module-design/protocols.md) |
| SSE Protocol | [Protocols](../module-design/protocols.md) |
| Message Queue / Event Bus | [Messaging](../module-design/messaging.md) |
| Admin Dashboard | [App Layer](../module-design/app.md) |
| Database Drivers | [Data Layer](../module-design/data.md) |
| Server Multi-Worker & Shutdown | [Server Core](../module-design/server.md) |
