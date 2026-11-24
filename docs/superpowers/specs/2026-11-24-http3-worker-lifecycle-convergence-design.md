# HTTP/3 (QUIC) 协议栈与 Multi-Worker 连接池全生命周期收敛设计规格说明书 (Design Specification)

## 1. 概述与背景

当前 `csilk` 已在 [src/protocols/http3.c](file:///data/home/quintin/workspace/source/c/csilk/src/protocols/http3.c) 中完成了基于 `ngtcp2` 与 `nghttp3` 的 HTTP/3 协议数据面解析与会话流调度。然而，当前实现存在两个显著的架构孤岛：
1. **网络层孤岛**：UDP 套接字由全局单一 `csilk_h3_transport_t` 独立创建并以局部非阻塞 `recvfrom` 轮询驱动，未利用 `SO_REUSEPORT` 和 `csilk_io_udp_t` 抽象挂载至 Multi-Worker 的各个事件循环中，无法实现网卡多队列硬件级散列（RSS）与多核多线程线性扩展。
2. **连接生命周期孤岛**：HTTP/3 连接中的 `csilk_client_t` 采用裸堆内存 `calloc` / `free`，完全绕过了 Worker 的无锁代际连接池（`wp->client_pool`），缺失 Generation Tag 代数防 ABA/UAF 保护，未挂载至 `wp->active_clients`，缺乏连接空闲/请求超时定时器保护，且在优雅停机（Graceful Drain）过程中无法受控回收。

本设计旨在将 HTTP/3 (QUIC) 协议栈无缝收敛至 Multi-Worker 核心底座与代际连接池中，使其与 HTTP/1.1、HTTP/2 享有同等的一等公民待遇（First-Class Citizen）。

---

## 2. 总体架构设计

```
                      +---------------------------------------+
                      |         Linux Kernel (UDP 443)        |
                      |          SO_REUSEPORT Hashing         |
                      +---------------------------------------+
                                  /               \
              UDP Datagrams      /                 \    UDP Datagrams
                                v                   v
          +-----------------------------+   +-----------------------------+
          |      Worker Thread 0        |   |      Worker Thread N        |
          |  (Pinned Core 0, loop_ptr)  |   |  (Pinned Core N, loop_ptr)  |
          +-----------------------------+   +-----------------------------+
          |  • wp->h3_udp_handle        |   |  • wp->h3_udp_handle        |
          |    (csilk_io_udp_recv_start)|   |    (csilk_io_udp_recv_start)|
          |  • wp->h3_transport         |   |  • wp->h3_transport         |
          |    (Worker-confined QUIC)   |   |    (Worker-confined QUIC)   |
          |  • ngtcp2 + nghttp3 Session |   |  • ngtcp2 + nghttp3 Session |
          +-----------------------------+   +-----------------------------+
          |  Worker-Local Lock-Free Pool|   |  Worker-Local Lock-Free Pool|
          |  • pool_get(wp)             |   |  • pool_get(wp)             |
          |  • Generation Tag (++gen)   |   |  • Generation Tag (++gen)   |
          |  • wp->active_clients       |   |  • wp->active_clients       |
          |  • cl->timer (Idle Timeout) |   |  • cl->timer (Idle Timeout) |
          |  • pool_get_arena(wp)       |   |  • pool_get_arena(wp)       |
          +-----------------------------+   +-----------------------------+
                        |                                 |
                        v                                 v
          +-----------------------------+   +-----------------------------+
          |   _csilk_dispatch_request   |   |   _csilk_dispatch_request   |
          |  (Router -> Handlers -> H3) |   |  (Router -> Handlers -> H3) |
          +-----------------------------+   +-----------------------------+
```

---

## 3. 详细设计与关键组件

### 3.1 Worker 级 UDP 句柄与 SO_REUSEPORT 分流
1. **数据结构扩展**：
   在 [src/core/internal/srv_internal.h](file:///data/home/quintin/workspace/source/c/csilk/src/core/internal/srv_internal.h) 的 `worker_pool_t` 结构体中新增：
   ```c
   csilk_io_udp_t h3_udp_handle;     /**< Worker-local UDP listen handle for HTTP/3 */
   bool           h3_udp_active;     /**< True if h3_udp_handle is bound and receiving */
   void*          h3_transport;      /**< Worker-local csilk_h3_transport_t instance */
   ```
   并在 `csilk_server_t` 中维护共享的：
   ```c
   int   h3_port;                   /**< Configured HTTP/3 UDP port (e.g. 443) */
   void* h3_ssl_ctx;                /**< Shared OpenSSL SSL_CTX* for QUIC */
   ```

2. **多线程并发绑定**：
   在 [src/core/server/server_worker.c](file:///data/home/quintin/workspace/source/c/csilk/src/core/server/server_worker.c) 的 `worker_thread` 流程中：
   - 当检测到服务器启用了 HTTP/3（`server->h3_port > 0` 或 `server->h3_transport != NULL`）：
     - 调用 `csilk_io_udp_init(loop_ptr, &wp->h3_udp_handle)` 初始化句柄；
     - 创建支持 `SO_REUSEPORT`（Linux `SO_REUSEPORT`，通过 `csilk_io_udp_open` 或带 reuseport 标志的 bind）的 UDP 套接字；
     - 绑定至 `0.0.0.0:h3_port`；
     - 调用 `csilk_io_udp_recv_start(&wp->h3_udp_handle, on_worker_h3_datagram_recv)` 启动事件驱动数据包接收。

3. **跨平台兼容与回退**：
   - 在支持 `SO_REUSEPORT` 的 Linux 系统上，所有 Worker 独立监听同一端口由内核调度；
   - 在单 Worker 或不支持 `SO_REUSEPORT` 的平台（如部分旧环境），由 Worker 0 独立持有 UDP 句柄。

### 3.2 真正纳入 Worker 代际连接池 (`client_pool`)
1. **连接分配 (`_h3_new_conn`)**：
   - 彻底废除裸 `c->client = calloc(1, sizeof(csilk_client_t))`；
   - 改为调用 `pool_get(wp)` 从 Worker 本地缓存池中分配 `csilk_client_t`；
   - 此时 `pool_get` 自动递增并赋与单调递增的 `generation` 标号，初始化原子引用计数；
   - 关联属性：
     ```c
     client->protocol = CSILK_PROTO_HTTP3;
     client->h3_conn = c;
     client->server = server;
     client->owner_pool = wp;
     csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
     ```
   - 内存分配：从 Worker 本地 Arena 池获取预热 Arena（`client->ctx.arena = pool_get_arena(wp)`）；
   - 链表管理：调用 `client_list_add(server, client)` 将其挂入 `wp->active_clients`，使连接被纳入活跃连接计数和监控列表。

2. **超时机制装载**：
   - 初始化并启动空闲 Keep-Alive 定时器：`csilk_io_timer_start(&client->timer, on_h3_conn_timeout, idle_timeout_ms, 0)`；
   - 当收到 HTTP/3 数据帧时重置该定时器；若超出空闲时间未有数据到达，触发优雅断开。

3. **连接销毁与代际回收 (`_h3_del_conn`)**：
   - 彻底废除裸 `free(c->client)`；
   - 状态流转：设置 `csilk_conn_set_state(client, CSILK_CONN_CLOSING)`；
   - 停止定时器：`csilk_io_timer_stop(&client->timer)` 与 `csilk_io_timer_stop(&client->request_timer)`；
   - 链表解绑：调用 `client_list_remove(server, client)` 从 `wp->active_clients` 摘除；
   - 归还 Arena：调用 `pool_put_arena(wp, client->ctx.arena)` 归还 Arena 池；
   - 归还 Client：调用 `pool_put(wp, client)` 触发 `reset_hot_state()` 并放入 `wp->client_pool` 供后续连接秒级复用。

### 3.3 请求分发与响应流式控制
1. **分发流水线**：
   - 在 [src/protocols/h3_session.c](file:///data/home/quintin/workspace/source/c/csilk/src/protocols/h3_session.c) 的 `csilk_h3_dispatch_stream` 中：
     - 状态置为 `csilk_conn_set_state(client, CSILK_CONN_PROCESSING)`；
     - 启动请求级超时定时器 `client->request_timer`（防止恶意慢速流）；
     - 调用核心路由分发器 `_csilk_dispatch_request(&client->ctx)`，完整走过中间件洋葱圈、路由匹配和业务 Handler。
2. **响应与回压流控**：
   - 响应由 `csilk_h3_send_response_for_ctx(&client->ctx)` 处理；
   - 如果发送缓冲区超出高水位线 `write_high_water_mark`（64KB），置 `ctx->write_paused = 1` 触发上游背压；
   - 当 ngtcp2/UDP 确认发送完成降至低水位 `write_low_water_mark`（16KB）时，触发 `csilk_on_drain` 回调继续生产。

### 3.4 优雅停机（Graceful Drain）收敛
1. **活跃连接遍历**：
   - 在 [src/core/server/server_shutdown.c](file:///data/home/quintin/workspace/source/c/csilk/src/core/server/server_shutdown.c) 的 `close_active_clients` 中支持 `client->protocol == CSILK_PROTO_HTTP3`；
   - 针对 HTTP/3 客户端，发送 QUIC `CONNECTION_CLOSE` / `NO_ERROR` 应用层代码，并安全触发 `_h3_del_conn`，不再尝试对非 TCP 的 `client->handle` 调用 `csilk_io_close`；
2. **UDP 句柄关闭**：
   - 在 Worker 退出流程中，调用 `csilk_io_udp_recv_stop(&wp->h3_udp_handle)` 并 `csilk_io_udp_close(&wp->h3_udp_handle, on_h3_udp_close)`，保证事件循环干净排空。

---

## 4. 质量保证与验证标准

1. **ABI 兼容门禁**：
   - 确保 `scripts/check_abi_compatibility.sh` 100% 通过（974+ 公共导出符号无损坏，不泄露内部符号）。
2. **单元测试与回归测试**：
   - 新增针对 Multi-Worker HTTP/3 连接池与生命周期的集成测试：
     - 验证 `pool_get` / `pool_put` 复用 `csilk_client_t` 且代数递增；
     - 验证 HTTP/3 连接进入 `wp->active_clients`；
     - 验证 HTTP/3 连接空闲超时自动关闭并回收；
     - 验证停机时 `close_active_clients` 正常清理 H3 连接且无内存泄漏与死锁。
3. **CI 矩阵验证**：
   - 必须通过全量 ASAN、TSAN、io_uring compatibility、ARM64 交叉编译及 macOS-15 / Ubuntu 24.04 编译与测试矩阵。
