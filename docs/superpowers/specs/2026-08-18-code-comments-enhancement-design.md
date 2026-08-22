# Csilk 代码库注释完善与 API 文档化设计规格

- **日期**：2026-08-18
- **版本**：v0.5.1
- **目标代码库**：Csilk (`csilk`) — 基于 C23 标准的高性能异步 Web 与智能体运行时

---

## 1. 目标与设计规范 (Goals & Documentation Standards)

### 1.1 总体目标
系统化推进全代码库的注释完善与文档化，涵盖：
1. **公共 API 头文件 (`include/csilk/`)**：达到工业级标准 Doxygen 文档化覆盖，清晰阐明函数简述、参数说明、返回值、内存所有权语义（Memory Ownership）与线程安全性约束（Thread-Safety）。
2. **核心源码实现 (`src/`)**：在复杂算法（SIMD、SWAR、HNSW、Kahn 拓扑排序）、状态机（Raft、Circuit Breaker、Workflow Resume）与并发临界区（Worker Confinement、Async Lease、LFQueue）深入加注设计意图（*Why*）与避坑说明（*Gotchas*）。
3. **零格式漂移与零功能退化**：严格遵循 `.clang-format` 代码格式，确保全套 172 项 CTest 单元测试 100% 通过。

### 1.2 Doxygen 注释标准模板
所有公共导出函数与结构体均采用如下规范格式：

```c
/**
 * @brief 简明一句话说明函数核心功能与目的。
 *
 * 详细描述函数的前置条件、算法行为、边界场景与潜在副作用。
 *
 * @param[in]     ctx      请求上下文句柄，非空。
 * @param[in]     key      存储键名，以 NUL 结尾。
 * @param[in]     val      动态分配的值对象指针。
 * @param[in]     dtor     析构回调函数，在上下文生命周期重置时自动执行；若无需释放可传 NULL。
 *
 * @return 成功返回 CSILK_OK (0)，参数无效返回 CSILK_ERR_INVALID (-1)，存储已满返回 CSILK_ERR_NOMEM (-2)。
 *
 * @note 内存所有权：val 指针所有权转移至 Context 托管，在请求 Arena 重置时由 dtor 安全释放。
 * @warning 线程安全性：严格限制由拥有该连接的 Worker 线程单线程访问，禁止跨线程并发调用。
 *
 * @see csilk_ctx_lease_acquire
 */
```

---

## 2. 分层加注规划与目标清单 (Layered Scope & File Targets)

### 2.1 Layer 1: Core Runtime 核心运行时
- **公共 API 头文件**：
  - `include/csilk/core/context.h`：上下文生命周期、参数提取、动态存储与异步租约 API 文档化。
  - `include/csilk/core/sys_io.h`：双后端（libuv / io_uring）事件循环、TCP 监听与定时器 API 文档化。
  - `include/csilk/core/types.h`：核心数据类型、Arena Chunk 分级阶梯与缓存对齐宏文档化。
  - `include/csilk/core/router.h` & `response.h`：路由注册、响应构建与背压流控函数文档化。
  - `include/csilk/core/sync.h`：跨后端线程同步原语文档化（强调 `csilk_barrier_t` 堆分配）。
- **核心实现源文件**：
  - `src/core/primitives/arena.c`：深入注释 Tier0(4K)/Tier1(16K)/Tier2(64K) 选择算法、TLS Free List 缓存链表与对齐逻辑。
  - `src/core/ctx/context.c` & `ctx_internal.h`：详细注释 `csilk_ctx_lease_acquire` / `release` 状态机与 RAII 析构链。
  - `src/core/uring/uring_run.c`：深度注释 SQPOLL 轮询、固定缓冲区环、Direct I/O 流程。
  - `src/core/server/server_worker.c`：详细注释 Worker 局域单线程约束与 `csilk_dispatch` 跨线程调度契约。
  - `src/core/primitives/router_simd.c` & `router_trie.c`：注释 AVX2 向量化分隔符比对与 Trie 树动态参数抽取。

### 2.2 Layer 2: Network & Protocols 协议栈与中间件
- **公共 API 头文件**：
  - `include/csilk/protocols/websocket.h`：WebSocket 全双工、帧发送、握手与事件回调 API 文档化。
  - `include/csilk/protocols/sse.h`：Server-Sent Events 流式事件广播 API 文档化。
  - `include/csilk/protocols/h3.h`：HTTP/3 与 QUIC 接口原型文档化。
  - `include/csilk/core/middleware.h`：洋葱中间件流水线、`csilk_next` 传递与短路机制文档化。
  - `include/csilk/middleware/otlp_trace.h`：OpenTelemetry 追踪导出 API 文档化。
- **核心实现源文件**：
  - `src/core/http/swar_http.c`：注释 64 位 SWAR 位掩码推导算法与并行边界检测。
  - `src/core/http/http1_zerocopy.c`：注释零拷贝切片指针与底层接收缓冲区的生命周期依赖。
  - `src/core/http/h2_callbacks.c` & `h2_session.c`：注释 nghttp2 多流复用与 HPACK 状态表管理。
  - `src/core/http/tls.c`：注释 OpenSSL ALPN 协议协商与非阻塞异步 BIO 缓冲区桥接。
  - `src/protocols/websocket.c` & `ws_room.c`：注释 RFC 6455 掩码并行异或与 Room 跨线程广播分发。
  - `src/middleware/circuit_breaker.c` & `src/core/primitives/response.c`：注释三态断路器与出站高水位背压（`csilk_on_drain`）。

### 2.3 Layer 3: Storage & Messaging 存储与分布式消息
- **公共 API 头文件**：
  - `include/csilk/drivers/db.h`：统一 DB 连接池、CRUD、参数绑定与事务接口文档化。
  - `include/csilk/drivers/vector.h`：向量数据库抽象、距离度量（L2/余弦）与 ANN 查询接口文档化。
  - `include/csilk/messaging/mq.h`：Pub/Sub 主题分发、消费者组与生产投递 API 文档化。
  - `include/csilk/messaging/raft.h` & `raft_wal.h`：Raft 共识节点与 WAL 预写日志接口文档化。
- **核心实现源文件**：
  - `src/drivers/db/db.c` & `sqlite.c`：注释连接池分配、自动重连与事务状态流转。
  - `src/drivers/vector/vector_hnsw.c` & `vector_simd.c`：注释 HNSW 跳表多层图贪心搜索与 AVX2 向量距离加速算子。
  - `src/messaging/mq_core.c` & `mq_pubsub.c`：注释无锁环形队列 (LFQueue) 与主题通配匹配逻辑。
  - `src/messaging/mq_wal.c` & `raft_wal.c`：注释二进制段文件追加写、CRC32 校验与崩溃恢复回放。
  - `src/messaging/raft_consensus.c` & `raft_snapshot.c`：注释 Leader 选举、Quorum 日志复制与快照压缩。

### 2.4 Layer 4: Workflow & AI 智能体与工作流
- **公共 API 头文件**：
  - `include/csilk/drivers/ai.h`：统一 LLM 客户端、对话生成、嵌入与流式事件接口文档化。
  - `include/csilk/protocols/mcp.h`：Model Context Protocol 服务端与客户端接口文档化。
- **核心实现源文件**：
  - `src/drivers/ai/ai.c` & `openai.c` & `ollama.c`：注释 OpenAI / Ollama 协议适配与 SSE 流式 Token 分发。
  - `src/protocols/mcp/mcp_server.c` & `mcp_jsonrpc.c`：注释 JSON-RPC 2.0 异步传输与工具 Schema 校验。
  - `src/workflow/wf_graph.c` & `wf_node.c`：注释 Kahn 拓扑排序、环路检测与多类型节点执行体系。
  - `src/workflow/wf_run.c`：注释异步就绪队列调度器与任务分发。
  - `src/workflow/wf_wal.c` & `wf_resume.c`：注释节点级 Checkpoint 快照与崩溃断点续传恢复状态机。

---

## 3. 质量与验证矩阵 (Quality & Verification Matrix)

每完成一个层级的代码注释更新，必须执行如下质量闭环：
1. **格式化与合规校验**：
   ```bash
   cmake --build build --target format
   cmake --build build --target check-format
   ```
2. **静态代码分析**：
   ```bash
   cmake --build build --target tidy
   ```
3. **全量单测回归**：
   ```bash
   ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
   ```
4. **版本同步校验**：
   ```bash
   ./scripts/check_version_sync.sh
   ```

---

## 4. 文档自检评审 (Spec Self-Review)

- **占位符检查**：无任何 `TBD`、`TODO` 或不明确要求。
- **一致性检查**：目标文件与路径完全真实存在于仓库中，严格遵循 C23 与 v0.5.1 标准。
- **范围清晰性**：按 Core、Protocols、Storage、Workflow 四大层次清晰切分，可按部就班分批推进。
