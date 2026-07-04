# 协议模块

## 1. 概述

协议模块通过 **WebSocket** (RFC 6453)、**Server-Sent Events** (SSE, RFC 8895)、**Swagger/OpenAPI UI** 以及 **WebSocket 房间** 系统扩展了 HTTP/1.1 服务器，提供实时、双向和交互能力。WebSocket 升级 **MUST** 在请求处理程序内完成且不阻塞。SSE 连接 **MUST** 使用分块传输编码，且 **MUST NOT** 缓冲。Swagger UI 资源 **SHOULD** 从嵌入静态数据提供。WebSocket 房间广播 **MUST** 在连接的客户端上为 O(n)，且 **SHOULD NOT** 阻塞发布线程。

**文件**: `src/protocols/websocket.c`, `src/protocols/swagger.c`, `src/protocols/ws_room.c`, `src/middleware/sse.c`

---

## 2. 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP 请求管道                               │
│                                                              │
│  ┌─────────────────────┐    ┌───────────────────────────┐   │
│  │ WebSocket            │    │ SSE                       │   │
│  │ ┌──────────────────┐ │    │ ┌────────────────────────┐│   │
│  │ │ csilk_ws_handshake│ │    │ │ csilk_sse_init()       ││   │
│  │ │ (101 Upgrade)     │ │    │ │ (200 + text/event-     ││   │
│  │ │ → is_websocket=1  │ │    │ │  stream headers)       ││   │
│  │ └────────┬─────────┘ │    │ │ → is_sse=1             ││   │
│  │          │           │    │ └───────────┬────────────┘│   │
│  │  ┌───────▼────────┐  │    │             │             │   │
│  │  │ 帧 I/O           │  │    │  ┌──────────▼──────────┐ │   │
│  │  │ csilk_ws_send() │  │    │  │ csilk_sse_send()    │ │   │
│  │  │ csilk_ws_parse  │  │    │  │ csilk_sse_close()   │ │   │
│  │  │  _frame()       │  │    │  └─────────────────────┘ │   │
│  │  │ csilk_ws_close()│  │    │                           │   │
│  │  └───────┬────────┘  │    └───────────────────────────┘   │
│  │          │           │                                     │
│  │  ┌───────▼────────┐  │    ┌───────────────────────────┐   │
│  │  │ WS 房间          │  │    │ Swagger UI                │   │
│  │  │ csilk_ws_join   │  │    │ csilk_serve_swagger_ui()  │   │
│  │  │  _room()        │  │    │ (嵌入 HTML + JS)         │   │
│  │  │ csilk_ws_leave  │  │    │ 加载 /openapi.json        │   │
│  │  │  _room()        │  │    └───────────────────────────┘   │
│  │  │ csilk_ws_broad  │  │                                     │
│  │  │  cast_room()    │  │                                     │
│  │  │ (基于 MQ)        │  │                                     │
│  │  └─────────────────┘  │                                     │
│  └───────────────────────┘                                     │
└─────────────────────────────────────────────────────────────┘
```

### 关键设计决策

1. **协议劫持 HTTP 套接字** — WebSocket 和 SSE 在初始握手后绕过正常的 HTTP 响应管道，直接写入底层的 libuv TCP/TLS 流，接管连接的全部生命周期。

2. **WebSocket 房间通过 MQ** — 房间广播系统利用内部消息队列 (MQ) 作为发布/订阅事件总线。每个房间映射到一个 MQ 主题（`ws.room.<name>`），从而解耦广播者和接收者。

3. **直接套接字写入** — 所有 SSE 和 WebSocket 帧写入都使用 `uv_write()` 直接在客户端流上进行，绕过 csilk 的正常响应写入。这避免了缓冲延迟并实现了实时吞吐量。

---

## 3. WebSocket (RFC 6455)

### 3.1 握手

```
客户端 → 服务器:  GET /ws HTTP/1.1
                 Upgrade: websocket
                 Connection: Upgrade
                 Sec-WebSocket-Key: <base64-random-16-bytes>
                 Sec-WebSocket-Version: 13

服务器 → 客户端:  HTTP/1.1 101 Switching Protocols
                 Upgrade: websocket
                 Connection: Upgrade
                 Sec-WebSocket-Accept: <base64(sha1(key + GUID)>
```

握手由 `csilk_ws_handshake()` 执行：

```
1. 读取 Sec-WebSocket-Key 头 → 如果缺失则返回 400
2. 计算 SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
3. Base64-编码 20 字节摘要 → Sec-WebSocket-Accept
4. 设置响应头（Upgrade, Connection, Accept）
5. 将状态设置为 101 (Switching Protocols)
6. 标记 ctx->is_websocket = 1
```

握手后，函数返回并期望处理程序进入由 libuv 的 `on_read` 回调驱动的读取循环。

### 3.2 帧 I/O

**发送** (`csilk_ws_send`):

```
csilk_ws_send(ctx, data, len, opcode):
  1. 确定掩码（0 对于服务器→客户端帧；客户端→服务器帧已掩码）
  2. 构建帧头:
     字节 0: FIN | opcode (例如，0x81 = 文本帧，0x82 = 二进制)
     字节 1: MASK | payload_len (根据长度是 7 位、16 位或 64 位)
     字节 2+: 扩展长度（如果适用）
     字节 N+: 掩码键（如果适用）
     字节 K+: 有效负载
  3. 在客户端流上调用 uv_write() （异步）
  4. 写入完成回调释放请求 + 缓冲区
```

**接收** (`csilk_ws_parse_frame`):

```
在 ctx->is_websocket 设置时从 on_read() 调用:
  1. 解析帧头（FIN, opcode, MASK, length）
  2. 如果已掩码：使用 XOR 解除有效负载掩码
  3. 按 opcode 分发:
     - 0x1 (text)   → 调用用户 on_message 回调
     - 0x8 (close)  → 发送关闭帧，关闭连接
     - 0x9 (ping)   → 发送 pong 帧
     - 0xA (pong)   → 无操作
```

**关闭** (`csilk_ws_close`):

发送关闭帧（opcode 0x8）带有可选状态码和原因。根据 RFC 6455，关闭握手是双向的 — 发起者发送关闭，对等方必须响应自己的关闭帧，之后 TCP 连接可以关闭。

### 3.3 API

| 函数 | 角色 |
|---|---|
| `csilk_ws_handshake(ctx)` | 执行 HTTP→WS 升级 (101) |
| `csilk_ws_send(ctx, data, len, opcode)` | 发送 WebSocket 数据帧（文本/二进制） |
| `csilk_ws_parse_frame(ctx, data, len)` | 解析并分发传入帧 |
| `csilk_ws_close(ctx, status, reason)` | 发起关闭握手 |
| `csilk_ctx_set_websocket(ctx, 1)` | 将上下文标记为 WebSocket 模式 |

### 3.4 回调注册

用户通过上下文 API 注册 on_message 回调：

```c
void on_ws_message(csilk_ctx_t* ctx, const uint8_t* data, size_t len, int opcode) {
    // 处理接收到的消息
    csilk_ws_send(ctx, data, len, 0x1); // 作为文本回显
}

void ws_handler(csilk_ctx_t* ctx) {
    csilk_ws_handshake(ctx);
    ctx->on_ws_message = on_ws_message;
}
```

该回调在 `csilk_ws_parse_frame()` 在事件循环线程上每次接收到完整的文本或二进制帧时调用。

---

## 4. WebSocket 房间

**文件**: `src/protocols/ws_room.c`

### 架构

```
┌─────────────────────┐     MQ 主题 "ws.room.chat"     ┌─────────────────────┐
│  发布者             │ ──────────────────────────────→  │  房间 "chat"         │
│  (任何处理程序)      │                                  │  ┌────────────────┐ │
│                     │                                  │  │ 客户端 A (WS)  │ │
│                     │                                  │  │ 客户端 B (WS)  │ │
│                     │                                  │  │ 客户端 C (WS) │ │
│                     │                                  │  └────────────────┘ │
│                     │                                  │  MQ 订阅者:      │
│                     │                                  │  on_room_message() │
│                     │                                  │  → ws_send 所有     │
└─────────────────────┘                                  └─────────────────────┘
```

### API

| 函数 | 角色 |
|---|---|
| `csilk_ws_join_room(ctx, room_name)` | 将客户端加入房间，如果新建则创建房间 + MQ 订阅者 |
| `csilk_ws_leave_room(ctx, room_name)` | 从房间移除客户端（O(1) 交换到末尾） |
| `csilk_ws_broadcast_room(ctx, room_name, message)` | 通过 MQ 主题 `ws.room.<name>` 发布到房间 |

### 房间管理器

```c
ws_room_manager_t:
  ├── rooms[] → ws_room_t:
  │               ├── room_name (strdup'd)
  │               ├── clients[] (csilk_ctx_t* 指针)
  │               ├── clients_count / clients_capacity
  └── mutex (uv_mutex_t, 序列化所有房间操作)
```

- 房间在第一次 `csilk_ws_join_room()` 调用时延迟创建。
- 每个房间在主题 `ws.room.<name>` 上创建一个 MQ 订阅者，将消息广播到房间内的所有客户端。
- 客户端移除使用交换到末尾实现 O(1) 删除。
- 通过 `g_room_manager.mutex` 实现线程安全。

### 广播流程

```
csilk_ws_broadcast_room(ctx, "chat", "hello"):
  1. csilk_mq_publish(mq, "ws.room.chat", "hello", 5)
  2. MQ 分发 → on_room_message(mq_ctx):
     a. 从主题提取房间名称（跳过 "ws.room." 前缀）
     b. 锁定房间管理器互斥锁
     c. 按名称查找房间
     d. 对于每个客户端: csilk_ws_send(client, payload, len, 0x1)
     e. 解锁房间管理器互斥锁
```

---

## 5. Server-Sent Events (SSE, RFC 8895)

**文件**: `src/middleware/sse.c`

### 连接流程

```
csilk_sse_init(ctx):
  1. 设置头: Content-Type: text/event-stream
                   Cache-Control: no-cache
                   Connection: keep-alive
                   X-Accel-Buffering: no
  2. 标记 ctx->is_sse = 1
  3. 通过 uv_write() 将原始 HTTP 200 OK 响应头 + 上述头直接写入客户端套接字
```

SSE 处理程序 **不** 调用 `csilk_next()` — 它劫持套接字进行整个连接生命周期。

### 事件格式

```
event: update\n
data: {"key":"value"}\n
\n
```

根据 RFC 8895 §2，每条事件由可选的 `event:` 和 `data:` 字段组成，由空行终止。

### API

| 函数 | 角色 |
|---|---|
| `csilk_sse_init(ctx)` | 初始化 SSE 连接，写入头 |
| `csilk_sse_send(ctx, event, data)` | 发送 SSE 事件（可选事件类型） |
| `csilk_sse_close(ctx)` | 通过 uv_close() 关闭 SSE 连接 |

### 优雅关机

在服务器关机期间，`server.c` 中的 `close_active_clients()` 通过 `ctx->is_sse` 检测 SSE 连接并在关闭前发送关闭事件：

```c
if (client->ctx.is_sse) {
    csilk_sse_send(&client->ctx, "close", "Server stopping");
    csilk_sse_close(&client->ctx);
}
```

---

## 6. Swagger / OpenAPI UI

**文件**: `src/protocols/swagger.c`

### 实现

`csilk_serve_swagger_ui()` 提供一个嵌入 HTML 页面，包含 Swagger UI 发行版（编译为二进制字符串常量）。该页面在运行时从同一服务器加载 `/openapi.json` 以渲染交互式 API 文档。

```
csilk_serve_swagger_ui(ctx):
  1. 设置 Content-Type: text/html; charset=utf-8
  2. csilk_string(ctx, 200, swagger_ui_html)
```

### OpenAPI 规范生成

OpenAPI 规范（`/openapi.json`）是从路由元数据动态生成的。使用 `csilk_app_add_route_extended_perm()` 或 Python 的 `app.route()` 注册的路由采用 `summary`/`description`/`input_type`/`output_type` 参数参数自动填充 OpenAPI 规范。JSON 通过一个内置处理程序提供，与 Swagger UI 注册在一起的。

### 注册

在 App 层，Swagger UI 和 OpenAPI 会自动注册：

```c
// 在 app.c 的 app 创建期间:
csilk_app_add_route(app, "GET", "/swagger", swagger_handler, 1);
csilk_app_add_route(app, "GET", "/openapi.json", openapi_handler, 1);
```

---

## 7. read() 集成

WebSocket 帧解析已集成到核心连接读取路径中：

```
on_read(stream, nread, buf):
  if ctx->is_websocket:
      csilk_ws_parse_frame(&client->ctx, base, nread)
  else:
      llhttp_execute(...)    ← 正常 HTTP 解析
```

当 `ctx->is_websocket` 被设置（在握手后），传入的 TCP 数据直接路由到 WebSocket 帧解析器，而不是 HTTP 解析器。这发生在 libuv 读取回调级别，没有中间缓冲。

---

## 8. 并发模型

- **WebSocket I/O** — 所有帧写入和读取都发生在事件循环线程上。没有并发访问客户端流。
- **房间** — 由 `g_room_manager.mutex` 保护。房间操作（加入/离开）和广播都通过同一个锁序列化。实际的广播工作（ws_send）在锁下完成。
- **SSE** — 事件循环上的单线程。每个 `sse_send()` 分配、写入和释放都在同一个线程上进行。
- **Swagger** — 提供静态 HTML 的无状态处理程序。没有并发问题。

---

## 9. 相关文件

| 文件 | 角色 |
|---|---|
| `src/protocols/websocket.c` | WebSocket 握手、帧发送/解析/关闭 |
| `src/protocols/ws_room.c` | 基于 MQ 广播的房间管理 |
| `src/protocols/swagger.c` | 嵌入 Swagger UI HTML 提供 |
| `src/middleware/sse.c` | 原始 libuv 流上的 SSE 初始化/发送/关闭 |
| `src/core/connection.c` | is_websocket 时路由到 WS 解析器的 on_read() |
| `src/core/server.c` | WebSocket/SSE 连接的优雅关机 |
| `src/core/context.c` | ctx->is_websocket / is_sse 标志和设置器 |
| `include/csilk/csilk.h` | WebSocket/SSE 公共 API 声明 |
| `python/csilk/context.py` | Python SSE 绑定（sse_init, sse_send, sse_close） |

---

## 10. 设计理由

**为什么 SSE/WS 使用原始套接字写入而不是正常响应管道？**  
SSE 和 WebSocket 连接是长期有效的，需要流式写入而不缓冲。正常的 csilk 响应管道会在刷新前缓冲完整的响应体，这与实时协议不兼容。直接的 `uv_write()` 调用绕过此缓冲并让协议代码完全控制帧定时。

**为什么为 WebSocket 房间使用 MQ 而不是直接客户端列表？**  
使用 MQ 解耦了广播者和接收者。任何组件（HTTP 处理程序、AI 工作流、计划作业）都可以通过向 MQ 发布消息来广播到房间 — 它不需要访问房间的客户端列表或 WebSocket 基础设施。MQ 还通过单个主循环分发点实现了工作线程之间房间通信的能力。

**为什么嵌入 Swagger UI HTML？**  
从嵌入字符串提供 Swagger UI 消除了文件系统依赖并简化了部署 — 服务器二进制文件是自包含的，无需与其一起分发静态资源。