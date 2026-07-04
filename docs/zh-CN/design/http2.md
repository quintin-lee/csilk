# HTTP/2 集成 — 实现状态

> **状态**: 第一阶段（Session 框架）、第二阶段（请求分发/响应）和第三阶段（Server Push）已完成。  
> **版本**: v0.3.0+ | **最后更新**: 2026-05-31
>
> **HTTP/2 规则**: ALPN 协商 **必须** 在任何数据路由之前完成。流上下文 **必须** 使用 arena 分配 — 每个流无需单独的 `malloc`/`free`。Server Push **不得** 在 HTTP/1.1 连接上通告。HPACK 动态表大小 **应该** 按部署调整（默认：nghttp2 4096 字节）。

## 1. 概述
HTTP/2（RFC 7540）引入了二进制帧、多路复用和头部压缩（HPACK）。csilk 框架已集成 `nghttp2` 用于帧解析和生成。

## 2. 实现状态

### 第一阶段 — Session 框架 ✅
- **ALPN 协商**：`src/core/server.c` 配置 OpenSSL ALPN 以提供 `h2` 和 `http/1.1`。TLS 握手后，`alpn_select_cb` 检测协商的协议并设置 `client->protocol`。
- **nghttp2 session**：`csilk_h2_init_session()` 以服务器模式创建 nghttp2 session，注册回调（`on_header`、`on_frame_recv`、`on_data_chunk_recv`、`on_stream_close`、`send_callback`），并配置标准 HTTP/2 设置（最大并发流数、初始窗口大小）。
- **数据路由**：ALPN 协商后，`process_tls_read()` 将解密数据路由到 `csilk_h2_process_data()`（用于 HTTP/2 连接）或 `llhttp`（用于 HTTP/1.1）。
- **`csilk_h2.h` 公共 API**：暴露 `csilk_h2_init_session`、`csilk_h2_process_data`、`csilk_h2_get_or_create_stream`、`csilk_h2_free_streams`。

### 第二阶段 — 请求分发和响应 ✅
- **统一分发**：从 HTTP/1.1 的 `on_message_complete` 处理器中提取 `_csilk_dispatch_request()`。两个协议现在共享相同的路由、中间件链和钩子触发逻辑。
- **头部解析**（`on_header_callback`）：将 `:method`、`:path` 伪头部和常规头部解析到 arena 后备的 `csilk_request_t` 字段中（method、path、query_params、headers）。
- **帧完成**（`on_frame_recv_callback`）：当 HEADERS 或 DATA 帧上设置了 `NGHTTP2_FLAG_END_STREAM` 时，通过 `_csilk_dispatch_request()` 分发请求。
- **请求体积累**（`on_data_chunk_recv_callback`）：传入的 DATA 帧载荷被连接到 `c->request.body` 中（堆 realloc，上限为 `max_body_size`）。
- **流清理**（`on_stream_close_callback`）：从链表中移除流上下文，调用 `csilk_ctx_cleanup()`，释放 arena，并释放上下文结构体。
- **响应发送**（`csilk_h2_send_response`）：构建带有 `:status` 伪头部和响应头的 nghttp2 HEADERS 帧，然后通过 `nghttp2_data_provider` 回调（`body_read_callback`）发送 DATA 帧。

### 第三阶段 — Server Push ✅
- **`csilk_push_promise`**：用于向客户端发送 `PUSH_PROMISE` 帧的公共 API。从 HTTP/2 请求处理器中调用，在资源被请求前推送资源。在 HTTP/1.1 连接上，此操作为空操作。
- **响应分发**：推送的资源通过路由器自动分发，响应在承诺的流上发送。

### 剩余工作（未来阶段）
- **流量控制**：流/连接级别流量控制的窗口更新管理。
- **流优先级**：依赖树调度。
- **HPACK 动态表**：nghttp2 内部处理此功能，但调整表大小可能改善压缩。
- **连接前导和 SETTINGS**：目前使用 nghttp2 默认值 — 可能需要自定义。

## 3. 架构

### 3.1 连接分发器
`on_read` 和请求处理器之间的新层：
- 如果 ALPN = `h2`，将数据路由到 `csilk_h2_process_data()`（nghttp2 帧解析器）。
- 如果 ALPN = `http/1.1` 或无 ALPN，路由到 `llhttp`。

### 3.2 流映射
- 每个 H2 流 ID 通过 `csilk_h2_get_or_create_stream()` 映射到一个 `csilk_ctx_t`。
- 流上下文存储在 `csilk_client_t::h2_streams` 的单向链表中。
- 在一个 TCP 连接上可以同时有多个活跃流。

## 4. 依赖
- **nghttp2**（v1.52+）：帧解析、HPACK、session 管理。
- **OpenSSL**：支持 ALPN 扩展的 TLS 1.3。

## 5. 性能说明
- nghttp2 使用动态表在内部处理 HPACK；不需要手动头部压缩。
- 流上下文使用与 HTTP/1.1 上下文相同的 arena 分配器模型，实现零碎片内存管理。
- 统一的 `_csilk_dispatch_request` 确保两个协议共享相同的优化基数树路由器和中间件链。
