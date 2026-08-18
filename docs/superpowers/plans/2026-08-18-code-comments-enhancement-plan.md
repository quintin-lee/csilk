# Csilk 代码库注释完善与 API 文档化实施计划 (Implementation Plan)

- **日期**：2026-08-18
- **版本**：v0.4.0
- **关联设计文档**：[docs/superpowers/specs/2026-08-18-code-comments-enhancement-design.md](file:///home/quintin/Data/source/c_cpp/server-c/docs/superpowers/specs/2026-08-18-code-comments-enhancement-design.md)
- **目标**：分 4 大阶段逐步推进全库公共 API 头文件 Doxygen 文档化与核心源文件关键路径算法/并发注释。

---

## 阶段一：Core Runtime 核心运行时加注 (Layer 1)

### 任务 1.1：公共 API 头文件 Doxygen 完善
- [ ] 完善 `include/csilk/core/context.h` 的函数说明、参数方向、返回值与线程安全性标注
- [ ] 完善 `include/csilk/core/types.h` 与 `include/csilk/core/sys_io.h` 的数据结构、宏定义与接口说明
- [ ] 完善 `include/csilk/core/router.h`, `response.h`, `sync.h` 的 Doxygen 注释（强调 `csilk_barrier_t` 堆分配）

### 任务 1.2：内存体系与上下文实现加注
- [ ] 深度加注 `src/core/primitives/arena.c`（Tier0/Tier1/Tier2 选择、TLS Free List 缓存与对齐逻辑）
- [ ] 深度加注 `src/core/ctx/context.c` 与 `src/core/ctx/ctx_internal.h`（RAII 析构链表、Async Lease 租约状态机）

### 任务 1.3：I/O 调度与路由算法加注
- [ ] 深度加注 `src/core/uring/uring_run.c`（SQPOLL 轮询、固定缓冲区、Direct I/O 流程）
- [ ] 深度加注 `src/core/server/server_worker.c`（Worker-Local 单线程隔离与 `csilk_dispatch` 契约）
- [ ] 深度加注 `src/core/primitives/router_simd.c` 与 `router_trie.c`（AVX2 分隔符向量化比对与 Trie 参数截取）

### 任务 1.4：阶段一质量与格式回归
- [ ] 执行 `cmake --build build --target format` 与 `check-format`
- [ ] 执行 CTest 单测回归确保 100% 通过

---

## 阶段二：Network & Protocols 协议栈与中间件加注 (Layer 2)

### 任务 2.1：协议公共 API 头文件 Doxygen 完善
- [ ] 完善 `include/csilk/protocols/websocket.h`, `sse.h`, `h3.h` 的 Doxygen 注释与背压返回值说明
- [ ] 完善 `include/csilk/core/middleware.h` 与 `include/csilk/middleware/otlp_trace.h` 的接口说明

### 任务 2.2：HTTP 解析与零拷贝流水线加注
- [ ] 深度加注 `src/core/http/swar_http.c`（64位 SWAR 位掩码推导算法与并行边界检测）
- [ ] 深度加注 `src/core/http/http1_zerocopy.c` 与 `http1_parse.c`（零拷贝切片生命周期与 Pipeline 管道化）
- [ ] 深度加注 `src/core/http/h2_callbacks.c`, `h2_session.c` 与 `tls.c`（nghttp2 多流复用与 ALPN 协商）

### 任务 2.3：WebSocket、中间件与背压流控加注
- [ ] 深度加注 `src/protocols/websocket.c` 与 `ws_room.c`（RFC 6455 掩码并行异或与 Room 跨线程广播）
- [ ] 深度加注 `src/middleware/circuit_breaker.c` 与 `src/core/primitives/response.c`（三态断路器与出站背压 `csilk_on_drain`）

### 任务 2.4：阶段二质量与格式回归
- [ ] 执行 `cmake --build build --target format` 与 `check-format`
- [ ] 执行 CTest 单测回归确保 100% 通过

---

## 阶段三：Storage & Messaging 存储与消息加注 (Layer 3)

### 任务 3.1：存储与消息公共 API 头文件 Doxygen 完善
- [ ] 完善 `include/csilk/drivers/db.h` 与 `vector.h` 的统一连接池与向量距离查询接口注释
- [ ] 完善 `include/csilk/messaging/mq.h`, `raft.h`, `raft_wal.h` 的主题分发与 Raft 接口注释

### 任务 3.2：数据库与向量检索引擎加注
- [ ] 深度加注 `src/drivers/db/db.c`, `sqlite.c`, `postgres.c`（连接池分配、自动重连与事务状态流转）
- [ ] 深度加注 `src/drivers/vector/vector_hnsw.c` 与 `vector_simd.c`（HNSW 跳表多层图贪心搜索与 AVX2 距离算子）

### 任务 3.3：消息队列、WAL 与 Raft 共识加注
- [ ] 深度加注 `src/messaging/mq_core.c` 与 `mq_pubsub.c`（无锁 LFQueue 消息环与主题扇出）
- [ ] 深度加注 `src/messaging/mq_wal.c` 与 `raft_wal.c`（二进制段文件追加写、CRC32 校验与故障回放）
- [ ] 深度加注 `src/messaging/raft_consensus.c` 与 `raft_snapshot.c`（Leader 选举、Quorum 日志复制与快照压缩）

### 任务 3.4：阶段三质量与格式回归
- [ ] 执行 `cmake --build build --target format` 与 `check-format`
- [ ] 执行 CTest 单测回归确保 100% 通过

---

## 阶段四：Workflow & AI 智能体加注 (Layer 4) 与最终验证

### 任务 4.1：智能体公共 API 头文件 Doxygen 完善
- [ ] 完善 `include/csilk/drivers/ai.h` 与 `include/csilk/protocols/mcp.h` 的 Doxygen 注释

### 任务 4.2：AI 驱动与 MCP 协议栈加注
- [ ] 深度加注 `src/drivers/ai/ai.c`, `openai.c`, `ollama.c`（统一 LLM 客户端与 SSE 流式 Token 转发）
- [ ] 深度加注 `src/protocols/mcp/mcp_server.c` 与 `mcp_jsonrpc.c`（JSON-RPC 2.0 异步传输与工具 Schema 路由）

### 任务 4.3：Workflow DAG 调度引擎与断点恢复加注
- [ ] 深度加注 `src/workflow/wf_graph.c`, `wf_node.c`, `wf_run.c`（Kahn 拓扑排序、节点执行体系与就绪队列调度）
- [ ] 深度加注 `src/workflow/wf_wal.c` 与 `wf_resume.c`（节点级 WAL Checkpoint 快照与崩溃断点续传恢复状态机）

### 任务 4.4：最终全量质量验收
- [ ] 执行全量 `clang-format` 格式校验
- [ ] 执行 `clang-tidy` 静态代码分析
- [ ] 执行 CTest 172 项单元测试回归
- [ ] 执行 `./scripts/check_version_sync.sh` 版本校验
