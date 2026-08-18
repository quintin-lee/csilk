# Csilk 代码库全景架构与模块深度剖析实施计划 (Implementation Plan)

- **日期**：2026-08-18
- **关联设计文档**：[docs/superpowers/specs/2026-08-18-codebase-deep-dive-analysis-design.md](file:///home/quintin/Data/source/c_cpp/server-c/docs/superpowers/specs/2026-08-18-codebase-deep-dive-analysis-design.md)
- **目标**：产出系统级详尽的 4 大专篇分析文档，并输出最终的技术白皮书与全量工程验证报告。

---

## 阶段一：专篇一 · Core Runtime 核心运行时深度分析编纂

### 任务 1.1：内存体系与 Arena 分配器解构
- [ ] 分析 `include/csilk/core/types.h` 中三级 Tier Chunk 机制（4K/16K/64K）
- [ ] 剖析 `src/core/primitives/arena.c` 的线程局部 TLS Free List 缓存与 `csilk_arena_reset`
- [ ] 总结 `csilk_arena_alloc()` vs `csilk_arena_calloc()` 规范与生命周期回收

### 任务 1.2：双后端事件循环与 I/O 抽象解构
- [ ] 剖析 `include/csilk/core/sys_io.h` 跨平台统一抽象层接口
- [ ] 深度解构 `src/core/uring/uring_run.c` 的 SQPOLL 内核线程轮询与 Direct Buffer 机制
- [ ] 梳理 Worker 线程调度与 `src/core/server/server_worker.c` 的 SO_REUSEPORT 模型

### 任务 1.3：上下文生命周期、异步租约与 Worker 局域隔离
- [ ] 分析 `src/core/ctx/context.c` 与 `src/core/ctx/ctx_internal.h` 的不透明句柄设计
- [ ] 梳理 `csilk_set_ex()` RAII 析构链表与 `csilk_defer`
- [ ] 详述 `csilk_ctx_lease_acquire()` / `release()` 异步租约生命周期保护机制
- [ ] 剖析 `wp->active_clients` 局域单线程隔离与 `csilk_dispatch()` 跨线程投递

### 任务 1.4：SIMD 向量化加速与 Radix/Trie 路由机制
- [ ] 剖析 `src/core/primitives/router_simd.c` 的 AVX2/SSE4.2 向量化路径分词
- [ ] 剖析 `src/core/primitives/router_trie.c` 的动态参数提取与通配匹配状态机

---

## 阶段二：专篇二 · Network & Protocols 网络与协议栈深度分析编纂

### 任务 2.1：HTTP/1.1 SWAR 位级扫描与零拷贝流水线
- [ ] 剖析 `src/core/http/swar_http.c` 的 64 位寄存器并行边界扫描
- [ ] 剖析 `src/core/http/http1_zerocopy.c` 的零拷贝切片与指针引用
- [ ] 梳理 `src/core/http/http1_parse.c` 与 Pipeline 管道化处理

### 任务 2.2：HTTP/2 多路复用、HTTP/3 原型与 TLS 1.3 协商
- [ ] 剖析 `src/core/http/h2_callbacks.c` 与 `h2_session.c` 的 nghttp2 状态机与 HPACK
- [ ] 剖析 `src/protocols/h3.c` 的 QUIC/UDP 异步流传输
- [ ] 剖析 `src/core/http/tls.c` 的 OpenSSL ALPN 握手协议协商

### 2.3：WebSocket 全双工与 Room 跨线程广播机制
- [ ] 剖析 `src/protocols/websocket.c` 的 RFC 6455 掩码运算与分片帧重组
- [ ] 剖析 `src/protocols/ws_room.c` 基于 `csilk_dispatch` 的 Worker-Confinement 广播路由

### 2.4：中间件洋葱流水线与出站高水位背压
- [ ] 剖析 `src/middleware/circuit_breaker.c` 的三态熔断降级状态机
- [ ] 剖析 `src/middleware/ratelimit.c` 与 `sliding_ratelimit.c`
- [ ] 剖析 `src/core/primitives/response.c` 的出站背压 `write_high_water_mark` 与 `csilk_on_drain`

---

## 阶段三：专篇三 · Storage & Messaging 存储与分布式消息深度分析编纂

### 任务 3.1：统一 DB 抽象与连接池管理
- [ ] 剖析 `src/drivers/db/db.c` 统一驱动抽象与事务控制
- [ ] 剖析 SQLite (`sqlite.c`)、PostgreSQL/MySQL (`postgres.c`, `mysql.c`) 与 Redis (`redis.c`) 实现

### 任务 3.2：纯 C HNSW 向量检索与 SIMD 算子
- [ ] 剖析 `src/drivers/vector/vector_hnsw.c` 的多层跳表图索引结构
- [ ] 剖析 `src/drivers/vector/vector_simd.c` 的 AVX2/NEON 欧氏/点积/余弦距离算子
- [ ] 梳理 Qdrant (`qdrant.c`) 与 Milvus (`milvus.c`) 远程驱动适配

### 任务 3.3：异步消息队列与 Pub/Sub 分发
- [ ] 剖析 `src/messaging/mq_core.c` 无锁环形队列 (LFQueue) 架构
- [ ] 剖析 `src/messaging/mq_pubsub.c` 的主题通配匹配与扇出
- [ ] 强调 `include/csilk/core/internal.h` 与 `messaging/mq_internal.h` 的隔离边界

### 任务 3.4：WAL 预写日志与 Raft 分布式共识
- [ ] 剖析 `src/messaging/mq_wal.c` 与 `raft_wal.c` 的 Segment 追加写与 CRC32 校验
- [ ] 剖析 `src/messaging/raft_consensus.c` 的 Leader 选举、心跳租约与 Quorum 提交
- [ ] 剖析 `src/messaging/raft_snapshot.c` 的快照生成与日志截断压缩

---

## 阶段四：专篇四 · High-Level Engine 智能工作流与代理引擎深度分析编纂

### 任务 4.1：原生 C 语言 AI LLM 驱动体系
- [ ] 剖析 `src/drivers/ai/ai.c` 统一大模型 API
- [ ] 剖析 `openai.c` 与 `ollama.c` 的协议交互与 SSE 流式 Token 转发

### 任务 4.2：MCP (Model Context Protocol) 协议栈
- [ ] 剖析 `src/protocols/mcp/mcp_jsonrpc.c` 的 JSON-RPC 2.0 异步传输
- [ ] 剖析 `src/protocols/mcp/mcp_server.c` 与 `mcp_client.c` 的 Resources/Prompts/Tools 实现

### 任务 4.3：Workflow DAG 拓扑调度引擎
- [ ] 剖析 `src/workflow/wf_graph.c` 的 Kahn 拓扑排序与环路检测
- [ ] 剖析 `src/workflow/wf_node.c` 与 `wf_tools.c` 节点体系
- [ ] 剖析 `src/workflow/wf_run.c` 的异步就绪队列调度

### 任务 4.4：声明式 DSL 与崩溃断点恢复机制
- [ ] 剖析 `src/workflow/workflow_dsl.c` 与 `workflow_loader.c` 的语法结构
- [ ] 剖析 `src/workflow/wf_wal.c` 节点级 Checkpoint
- [ ] 剖析 `src/workflow/wf_resume.c` 故障断点续传恢复状态机

---

## 阶段五：白皮书整合、超链接校验与全量工程验证

### 任务 5.1：生成全量架构白皮书
- [ ] 整合输出全量白皮书文档 `docs/architecture/csilk-codebase-deep-dive-report.md`
- [ ] 校验所有内部代码锚点与 `file://` 源码指针

### 任务 5.2：执行工程验证矩阵
- [ ] 执行 Clang Debug 构建与 CTest
- [ ] 执行 io_uring 后端构建与测试
- [ ] 执行 ASAN / TSAN 内存与竞态动态检测
- [ ] 执行代码格式与 clang-tidy 静态检查
