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
    participant Gzip MW as Gzip Middleware
    participant TP as libuv Thread Pool
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
| **Custom Middleware Development** | See below (本节新增) |
| **WebSocket Rooms 实现** | See below (本节新增) |
| **SSE 流式推送** | See below (本节新增) |
| **AI 工作流编排** | See below (本节新增) |
| **数据库事务管理** | See below (本节新增) |

---

## Custom Middleware Development (自定义中间件开发)

csilk 采用洋葱模型，请求自外向内经过中间件链。自定义中间件 MUST 遵循 `csilk_handler_t` 函数指针类型：

```c
typedef void (*csilk_handler_t)(csilk_ctx_t*, void*);

// 注册带参数的中间件
csilk_handler_t auth_handler = (csilk_handler_t)custom_auth_middleware;
csilk_set(c, "auth_role", "admin");  // 通过 Storage 传递参数

// 后续中间件可读取
csilk_str_view_t role = csilk_get(c, "auth_role");
```

**常见中间件模式**：

| 模式 | 用途 | MUST/SHOULD 规范 |
|:-----|------|-------------------|
| 认证 | JWT / Session 验证 | **MUST** 校验 Token 有效期和角色 |
| 验证 | 请求参数校验 | **SHOULD** 使用反射引擎（`csilk_bind_reflect`）自动绑定 |
| 限流 | 令牌桶算法 | **MUST** 设置 `Retry-After` Header |
| 日志 | 请求/响应记录 | **SHOULD** 记录 Request-Id 路径 |

### WebSocket Rooms 实现（基于 MQ）

WebSocket Rooms 实现跨连接的事件广播：

```c
// include/websocket/rooms.h
typedef struct {
    csilk_ws_t* ws;
    int client_fd;
    char room_id[64];
    bool is_active;
} ws_connection_t;

// 全局连接表（需加锁保护）
static struct {
    pthread_mutex_t lock;
    ws_connection_t* connections[4096];
    size_t count;
} rooms_ctx = {0};

int ws_rooms_join(csilk_ws_t* ws, const char* room_id) {
    pthread_mutex_lock(&rooms_ctx.lock);
    // 分配槽位、初始化连接、加入全局表
    // MUST 确保 thread-safe
    pthread_mutex_unlock(&rooms_ctx.lock);
    return 0;
}

int ws_rooms_broadcast(const char* room_id, const char* message, size_t len) {
    pthread_mutex_lock(&rooms_ctx.lock);
    // 遍历所有连接，查找属于该房间的连接
    // MUST 遍历时检查 is_active 标志
    pthread_mutex_unlock(&rooms_ctx.lock);
    return broadcast_count;
}
```

**路由注册**：
```c
csilk_handler_t ws_handlers[] = {ws_handler};
csilk_router_add(router, "GET", "/ws/{room_id}", ws_handlers, 1);
```

---

## SSE Stream Streaming (SSE 流式推送)

SSE 必须设置 `Content-Type: text/event-stream` 和 `Cache-Control: no-cache`：

```c
void sse_stream_handler(csilk_ctx_t* c) {
    csilk_sse_t* sse = csilk_sse_init(c);
    csilk_set_header(c, "Content-Type", "text/event-stream");
    csilk_set_header(c, "Cache-Control", "no-cache");
    csilk_set_header(c, "Connection", "keep-alive");

    csilk_sse_send(sse, "event: connected\ndata: connected\n\n", 24);

    for (int i = 0; i < 10; i++) {
        char buffer[512];
        int len = snprintf(buffer, sizeof(buffer),
            "event: update\ndata: {\"counter\": %d}\n\n", i);
        csilk_sse_send(sse, buffer, len);

        // 延长 keep-alive
        csilk_sse_extend_keepalive(sse, 5000);
        csilk_sleep(1000);
    }

    csilk_sse_close(sse);
}
```

**SSE 与 WebSocket 对比**：

| 维度 | SSE | WebSocket |
|:-----|:----:|:----------:|
| 协议 | HTTP | WebSocket |
| 双向通信 | 不支持 | **支持** |
| 兼容性 | 浏览器原生 | 浏览器原生 |
| 防火墙 | 常规 HTTP | 需手动配置 |
| 最佳场景 | 单向推送 | 双向实时通信 |

---

## AI Workflow Orchestration (AI 工作流编排)

csilk 的 AI 引擎支持工具调用（Function Calling），结合 Python 实现复杂工作流：

**C 端工具定义**：
```c
typedef int (*csilk_tool_fn_t)(csilk_ctx_t* c, const char* args, char* result, size_t result_size);

typedef struct {
    const char* name;
    const char* description;  // MUST 包含
    csilk_tool_fn_t fn;
} csilk_tool_t;
```

**Python 端调用示例**：
```python
tools = [
    Tool(
        name="weather",
        description="Get weather information for a city",
        fn=lambda args: subprocess.check_output(
            ["./csilk-tools", "weather", args]
        ).decode("utf-8")
    )
]

response = openai.ChatCompletion.create(
    model="gpt-4",
    messages=[{"role": "user", "content": user_message}],
    functions=[tool.to_openai_schema() for tool in tools],
    function_call="auto"
)

if response["choices"][0]["message"].get("function_call"):
    # 执行工具
    result = tool.fn(tool_args)
    # 生成最终响应
```

---

## Database Transaction Management (数据库事务管理)

使用 SQLite 事务保证数据一致性：

```c
int db_transaction_execute(sqlite3* conn, const char* sql) {
    // 1. 开始事务（MUST 使用 IMMEDIATE）
    sqlite3_exec(conn, "BEGIN IMMEDIATE TRANSACTION", NULL, NULL, NULL);

    // 2. 执行 SQL
    char* errmsg = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &errmsg);

    if (rc != SQLITE_OK) {
        // 回滚（MUST）
        sqlite3_exec(conn, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        fprintf(stderr, "SQL failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }

    // 3. 提交（SHOULD）
    sqlite3_exec(conn, "COMMIT TRANSACTION", NULL, NULL, NULL);
    return 0;
}
```

**批量事务**：
```c
int db_transaction_execute_json(sqlite3* conn, const char* queries) {
    // 1. 开始事务
    sqlite3_exec(conn, "BEGIN IMMEDIATE TRANSACTION", NULL, NULL, NULL);

    cJSON* json = cJSON_Parse(queries);
    cJSON* query_item = NULL;
    cJSON_ArrayForEach(query_item, json) {
        cJSON* sql_item = cJSON_GetObjectItem(query_item, "sql");
        if (sql_item && sql_item->valuestring) {
            // 逐条执行
            sqlite3_exec(conn, sql_item->valuestring, NULL, NULL, NULL);
        }
    }

    // 4. 提交或回滚
    if (success) {
        sqlite3_exec(conn, "COMMIT TRANSACTION", NULL, NULL, NULL);
    } else {
        sqlite3_exec(conn, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
    }

    cJSON_Delete(json);
    return success ? 0 : -1;
}
```

---

## Performance Optimization Tips (性能优化技巧)

### Zero-Copy Processing (零拷贝)

**避免 JSON 解析时的内存复制**：
```c
// 传统方式（有拷贝）
cJSON* json = cJSON_Parse(body);
char* json_str = cJSON_PrintUnformatted(json);  // 分配新内存
csilk_set(c, "json", json_str);
cJSON_Delete(json);

// 零拷贝方式（引用 buffer）
csilk_str_view_t body = csilk_get_body(c);
cJSON* json = cJSON_ParseWithLength(body.data, body.len);
csilk_set(c, "json", json);  // 传递指针，不复制
cJSON_Delete(json);
```

### Deferred Cleanup (延迟释放)

在 Recovery Handler 中保护资源：
```c
void custom_recovery(csilk_ctx_t* c) {
    csilk_set(c, "defer_pool", my_resource_pool);
    csilk_set(c, "defer_fd", my_file_descriptor);
    csilk_panic_recovery(c);
}

void custom_handler(csilk_ctx_t* c) {
    void* pool = csilk_get(c, "defer_pool");
    int fd = (int)(intptr_t)csilk_get(c, "defer_fd");
    if (pool) csilk_free(pool);
    if (fd >= 0) close(fd);
}
```

### Connection Reuse (连接复用)

```c
void on_request_end(csilk_ctx_t* c) {
    // 复用 Arena 而非销毁（SHOULD）
    csilk_arena_reset(c->arena);

    // 重置 Context
    csilk_ctx_reset(c);
}

void on_connection_close(csilk_ctx_t* c) {
    // 连接关闭时销毁资源（MUST）
    csilk_arena_free(c->arena);
}
```

---

## Configuration & Error Handling (配置与错误处理)

### Dynamic Configuration Hot Reload (动态配置热更新)

```c
void on_config_watch(csilk_ctx_t* c) {
    csilk_config_t new_config = csilk_config_load("config.yaml");
    csilk_server_set_config(c->server, &new_config);
}
```

### Custom Error Handler (自定义错误处理器)

```c
void custom_error_handler(csilk_ctx_t* c) {
    int status = csilk_get_status(c);
    cJSON* error = cJSON_CreateObject();

    cJSON_AddStringToObject(error, "error", "Internal Server Error");
    cJSON_AddNumberToObject(error, "status", status);

    csilk_json(c, status, error);
    cJSON_Delete(error);
}
```

---

## Best Practices (最佳实践)

| 实践 | 说明 |
|:-----|------|
| **MUST** 使用 `csilk_next(c)` 传递请求 | 中间件链必需步骤 |
| **SHOULD** 避免在热路径中 `malloc` | 使用 Arena 或预分配 |
| **SHOULD** 在 Recovery Handler 中清理资源 | 防止泄漏 |
| **MUST NOT** 直接修改 `csilk_ctx_s` | 间接通过 Accessor API |
| **MAY** 使用 `csilk_get/c` 传递中间件参数 | 简化函数签名 |
| **SHOULD** 长期连接设置 `keep_alive_timeout_ms` | 防止资源泄漏 |
| **MUST** TLS 1.3 用于生产环境 | 安全合规 |
