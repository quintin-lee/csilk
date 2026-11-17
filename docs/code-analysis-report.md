# csilk 全库代码分析报告

生成日期: 2026-11-17
基线: `master @ 5ab5f305` + Phase 1–4 修复
范围: `src/**`, `include/**`, `cmake/**`, `tests/**`
验证基线: 268 tests passed, `check-format` clean, `clang-tidy` clean

---

## 1. 仓库概览

### 1.1 语言 / 工具链
- C23 (CMAKE_C_STANDARD 23), 编译器: GCC 13+ / Clang 19+ (Apple Clang / MSVC 被显式拒绝)
- 构建: CMake ≥ 3.11, 依赖经 FetchContent: libuv|liburing, nghttp2, llhttp, yyjson, cJSON
- 可选驱动: OpenSSL (TLS/加密必需), sqlite3, libcurl, libpq, mysql, hiredis, mongoc, WAMR, yyjson
- 产物: `.a` + `.so`/`.dylib` 同步构建; 安装时生成 pkg-config `.pc`

### 1.2 顶层结构
```
include/csilk/**  公共头 (opaque handles)
src/core/**        server, ctx, primitives (arena/router/bounded_buf), http, io, uring, plugin, internal
src/middleware/**  23 个独立 .c 文件
src/protocols/**   websocket / http2 / swagger / mcp / mcp_stdio_pool / h3_stub
src/messaging/**   mq, pubsub, wal, raft, consensus, snapshot, lock
src/drivers/**     db (sqlite/mysql/pq/redis/mongo + stmt + tx_scope + migration),
                    ai (openai/anthropic/ollama/tool_registry/ai_trace/skill),
                    cipher/openssl, vector (hnsw + simd + milvus + qdrant + stub)
src/workflow/**    agent engine, DAG, DSL, react loop, skill, WAL, trace, scheduler, state
src/app/**         app.c, admin_*.c, groups, routes
src/reflection/**  marshal / unmarshal / free
src/util/**        flamegraph, csv
src/crypto/**      base64, sha1, bcrypt, blowfish, cipher, crypto dispatch
tests/**           单元测试 + 集成测试 (test_integration / sse / session / middleware_chain / openapi / admin)
fuzz/**            9 个 libFuzzer 目标 + corpus + dict
python/**          CFFI/ctypes bindings + setup.py + 测试
examples/**        6 个示例
cmake/**           options, targets, sources, tests, tooling, install, dependencies, format
```

### 1.3 模块依赖图 (来自 AGENTS.md)
```
workflow ──→ { core, json, ai, mq, wasm, yaml }
http       ──→ { core, json, tls, http2, mq, zlib, llhttp }
http2      ──→ { core, tls, nghttp2 }
db         ──→ { core, sqlite3, curl, mysql, pq, hiredis, mongoc }
ai         ──→ { core, json, curl }
tls        ──→ { core, openssl }
mq         ──→ core
wasm       ──→ core
bypass     ──→ core
core       ──→ { json, openssl, threads, {libuv|liburing}, yaml, m }
json       ──→ yyjson
```

---

## 2. 核心 (src/core)

### 2.1 事件循环抽象
- `src/core/io/*.c` 统一 `csilk_io_*` API, 由 `src/core/io_impl.c` + `src/core/io_uring.c` 两个后端实现
- **关键不变式** (本次修复引入): 所有 `csilk_io_timer_set/cancel` 回调必须运行在调用方所在 loop (跨后端对称):
  - libuv 路径: `uv_timer_init(loop, ...)` 绑定调用线程 loop
  - io_uring 路径: `_csilk_timer_set` 把 `loop->timers` 与 `timers_active` 都置为调用方 loop, 避免 `loop->timers` 仍指 owner 但 `timers_active` 已翻转为调用方的撕裂状态
- 违反不变式将导致: 计时器回调在 caller loop 中触发 → 访问 owner-loop 的 `wp->dispatch_async` (未同步) → TSAN 数据竞态; 或 owner-loop 中 `timers_active` 为 false → `timers_head` 为 NULL → 崩溃

### 2.2 连接池 & 状态机 (src/core/server)
拆分文件:
- `connection_pool.c` — client 池 + arena 池 + 读缓冲池; `_csilk_client_atomics_init` 集中初始化原子字段
- `connection_state.c` — 9 态状态机 (`CSILK_CONN_STATE_COUNT`), 转换矩阵 `allowed_transitions[9][9]`, 含不变式注释
- `connection_io.c` — accept/read/reject 回调; `on_rejected_close` 处理连接数溢出
- `connection_close.c` — `client_list_add/remove` 双向链表; generation 校验的关闭任务
- `connection_timer.c` — idle/write/read 超时回调; **关键修复**: idle 计时器取消必须使用持有 `client->loop` 的 loop, 否则 owner-loop 的 `timers_active` 仍指 owner, 导致崩溃
- `connection.c` — 薄兼容包装

### 2.3 服务器生命周期 (server_lifecycle.c / server_worker.c / server_shutdown.c)
- `csilk_server_new`: 初始化 `worker_pool_t` + TLS 缓存
- `csilk_server_run`: `csilk_spawn_workers` 派生 N 个 worker (SO_REUSEPORT + CPU pinning), `csilk_dispatch_tls_cache_t` 容量 64
- `csilk_server_stop`: 信号处理 → `close_active_clients` 无锁遍历 (H3 timing contract)
- `server_rcu.c` — RCU 槽获取/释放; `_csilk_reload_try_reclaim` 扫描静态 reader slot 最小活跃 epoch 后回收 retired router (Phase 3a 加入碰撞检测)
- `server_shutdown.c` — SIGINT → `csilk_server_stop`; worker 有序关闭, EBR 宽限期后销毁 router

### 2.4 Arena 分配器 (src/core/primitives/arena.c, 666 行)
- 三层 free-list: 4KB/16KB/64KB + `max_total_bytes` 上限
- TLS cleanup 注册 (`__thread`), chunk 链表
- `DEBUG_ARENA` redzone 检测; `csilk_arena_flush_free_list` 在 worker 退出时释放
- **不变式**: `csilk_arena_alloc` 未初始化 (零开销); 需要零值必须用 `csilk_arena_calloc` (本次修复已在 `wasm_vm.c` 中强化)

### 2.5 Router & RCU (src/core/primitives/router*.c)
- 分段前缀 trie (radix tree), `node_new` 设 `static_first=NULL` (惰性分配, Phase 3b), 首次插入静态子节点时 `malloc(256*sizeof(int16_t))`
- `router_trie.c` 迭代式非递归遍历, `router_stack_frame_t` 含 rollback 快照
- `router_simd.c` AVX2/NEON 加速 segment 提取
- RCU slot 读端 100% wait-free, 写端在 `config_mutex` 下保证 epoch 严格单调

### 2.6 Bounded buffer (src/core/primitives/bounded_buf.c, 502 行)
零堆分配的有界字符串/JSON builder, 溢出时截断 + 设置 overflow flag。被 JWT/JWT 验证 / WAF / OTLP / SSE 使用。

### 2.7 HTTP (src/core/http)
- `http1_parse.c` — llhttp 回调, 零拷贝 header 持久化, body 零拷贝
- `http1_write.c` — 写管线, sendfile, keep-alive 超时管理
- `swar_http.c` — SWAR (SIMD Word Arithmetic Regular) HTTP 解析加速
- `h2.c`, `h2_callbacks.c`, `h2_session.c`, `h2_response.c` — HTTP/2 支持 (nghttp2)
- `tls.c` — OpenSSL SSL_CTX, BIO-pair 加密 I/O, ALPN (h2/http1.1)
- `http1_pipeline.c`, `http1_zerocopy.c`, `http1_response.c` — 高级特性

### 2.8 请求上下文 (src/core/ctx/context.c, 900 行)
- body 所有权标记 (`CSILK_OWN_POOL/HEAP/TRANSFER/BORROWED/ARENA`)
- `csilk_ctx_defer` LIFO 清理链, `csilk_ctx_panic` 500 路径
- 异步模式 (`ctx_async.c`), accessor (`ctx_accessors.c`), JSON 便捷方法 (`ctx_json.c`)

### 2.9 WASM 插件 (src/core/plugin)
- `wasm_vm.c` — WAMR + stub 双路径, fuel 预算, `strnlen` + NUL 校验 (Phase 1 修复), 改用 `csilk_arena_calloc`
- `wasm_wasi.c` — WASI stub 限制文档 (Phase 4 补充)

### 2.10 同步原语
`src/core/sync.c` 暴露 `csilk_mutex_t` / `csilk_cond_t` / `csilk_barrier_*`, 后端由 libuv (uv_mutex_t/uv_barrier_t) 或 uring (pthread 等价物) 提供。`csilk_barrier_t` 必须堆分配。

---

## 3. 中间件 (src/middleware, 23 文件)

| 文件 | 职责 | 关键不变式 |
|---|---|---|
| `jwt.c` (733) | HS256/RS256/ES256 签发与验证 | 常量时间比较; 算法不匹配拒绝; **`payload` 必须 NUL-terminated** (本次修复) |
| `waf.c` | SQLi/XSS/目录遍历 | 命中后 `csilk_json_error` + `csilk_abort` |
| `sliding_ratelimit.c` (122) | 加权滑动窗口日志/计数器 | O(1) 窗口推进 |
| `sse.c` (257) | SSE 写完成回调 | 连接关闭时正确清理 |
| `circuit_breaker.c` | 熔断器 | 半开探测 |
| `xdp_waf.c` | eBPF XDP 动态 WAF | 仅内核旁路模式 |
| `otlp_exporter.c` / `otlp_trace.c` | OTLP APM | 环形缓冲 |
| `session.c` / `session_store.c` | 会话管理 | 后端可插 |
| 其余 (cors, csrf, security_headers, static, gzip, ratelimit, request_id, auth, metrics, logger, validate, multipart, grpc_gateway) | 常规 | 注册顺序执行 |

---

## 4. 协议 (src/protocols)

- `websocket.c` (409) — RFC 6455 握手 (SHA-1+Base64), 帧编码/解析, masking, close 帧
- `mcp_stdio_pool.c` — MCP 协议 stdio 客户端进程池
- `mcp.c` — MCP 协议层
- `h3_stub.c` — HTTP/3 / QUIC 占位
- `swagger.c` — OpenAPI 导出 (share/swagger-ui 资产)

---

## 5. 消息 (src/messaging)

- `mq.c` / `pubsub.c` / `wal.c` — 异步线程安全队列 + WAL
- `raft_consensus.c` (1309) — Raft UDP 共识
  - `CSILK_RAFT_MTU_BYTES` 默认 1400 (安全 1500B Ethernet MTU 内), CMake `#ifndef` 可覆盖
  - `raft_send_packet_locked` 返回 `int` (Phase 1 修复), 调用方允许丢弃
  - 随机选举、UDP RPC、日志复制
- `consensus.c` / `snapshot.c` / `lock.c` — 配套

---

## 6. 驱动 (src/drivers)

### 6.1 DB
- `sqlite.c` / `mysql.c` / `postgres.c` / `redis.c` / `mongodb.c` — 各驱动
- `stmt.c` — 原生 prepared statement, 类型化参数绑定
- `tx_scope.c` (95) — 嵌套事务/savepoint 自动降级
- `migration/*.c` — Flyway 风格迁移引擎 + 分布式锁

### 6.2 向量
- `vector_hnsw.c` (1005) — HNSW 多层索引, 反序列化边界校验 (`HNSW_MAX_DIM/NODES/M/DOC_LEN`)
- `vector_simd.c` — AVX2 距离内核 (cosine/L2/dot)
- `milvus.c` / `qdrant.c` — 远端服务客户端

### 6.3 AI
- `openai.c` / `anthropic.c` / `ollama.c` — LLM 客户端
- `tool_registry.c` — 工具定义 + schema 生成 + 分发
- `ai_trace.c` — 端到端延迟 / token 吞吐 / tool span
- `skill.c` — Agent Skill 抽象 (LOCAL bounded fork/execvpe, REMOTE via csilk_mq)

### 6.4 加密
- `cipher/openssl.c` — AES-256-GCM, RSA-OAEP, RSA-PSS
- `src/crypto/**` — base64, sha1, bcrypt (cost 10), blowfish sbox, cipher 分发
- JWT / WAF / OTLP 使用 `src/core/primitives/bounded_buf.c` 零堆分配

---

## 7. 工作流 / 应用 / 反射 / 工具

- `src/workflow/` — agent engine, DAG, DSL, react loop, skill, WAL, trace, scheduler, state
- `src/app/` — 高级 app API, admin handlers, groups, routes
- `src/reflection/` — struct↔JSON 反射 marshal/unmarshal/free
- `src/util/flamegraph.c` — 后向追溯采样 (coverage 排除)
- `src/util/csv.c` — RFC 4180 CSV 解析/序列化

---

## 8. 测试与质量

- 单元测试: 268 通过 (基线), 覆盖 `src/**` 主路径
- 集成测试: `test_integration`, `test_sse_integration`, `test_session_integration`, `test_middleware_chain_integration`, `test_openapi_integration`, `test_admin_integration`
- 扩展超时测试: `test_timeout` (10s), `test_multi_worker`/`test_mq_concurrent` (30s), `test_ws_concurrent`/`test_sse_concurrent` (15s)
- Sanitizer: ASAN (Linux), TSAN (`halt_on_error=1:print_suppressions=0`), coverage (gcc + gcovr, 当前 69%)
- Fuzzing: 9 个 libFuzzer 目标, 基线语料 `fuzz/corpus/`
- Lint: `clang-format` / `clang-tidy` 通过, 版本同步检查 (`check_version_sync.sh`)
- 文档: Mermaid 校验 (`check_mermaid.py`), Doxygen 生成

---

## 9. 已识别风险与修复 (本次会话)

### Phase 1: 即时修复
1. `raft_send_packet_locked` 返回类型 (void → int), 调用方检查返回值
2. `jwt.c` payload 构造 — NUL 终止 (cJSON 字符串可能无 NUL, 越界读)
3. `wasm_vm.c` — `csilk_arena_calloc` 替换 `csilk_arena_alloc`; 字符串参数 NUL 校验

### Phase 2a: 计时器跨线程对称
4. uring `csilk_io_timer_set` 回调线程不变式
5. uring `csilk_io_timer_cancel` 对称

### Phase 2b: 计时器调用方 loop
6. `server_shutdown.c` idle 计时器取消 loop 修正
7. `connection_timer.c` idle 计时器取消 loop 修正

### Phase 3a: RCU 碰撞检测
8. `_csilk_reload_try_reclaim` 静态 reader slot 碰撞告警

### Phase 3b: Router 节点内存
9. `static_first` 惰性堆分配 (节省 512 B/节点)

### Phase 4: 文档与不变式
10. `wasm_wasi.c` stub 限制文档
11. 各计时器调用方不变式注释 (libuv / uring 双路径对称)

---

## 10. 已知待处理

- `router_match.c` / `ctx.c` 文件名注释与实际 `router_trie.c` / `context.c` 不一致 (历史遗留)
- 部分模块 coverage < 50% (flamegraph.c, redis_storage.c, workflow_debug.c, uring_vector.c 已在 .gcovr 排除)
- Apple Silicon / Windows 不在 CI 矩阵
- `src/core/uring/` 是唯一的 uring 后端路径 (15 个文件), 不存在 `src/core/io_uring.c`; 报告中的 "uring 双轨" 为误判, 已修正
- 文件命名规范: `connection.c` 拆为 5 个子文件, 但头文件仍按单文件组织, 建议同步命名

---

## 11. 建议下一步

1. 修复文件头注释与实现文件命名不一致
2. 提升 vector / workflow / messaging 模块测试覆盖 (目标 > 80%)
3. 为 RCU 加 reader storm 压力测试 (已规划于 `test_rcu_reader_stress`)
4. 文档 `docs/en` / `docs/zh-CN` 同步更新 Phase 1–4 修复说明
