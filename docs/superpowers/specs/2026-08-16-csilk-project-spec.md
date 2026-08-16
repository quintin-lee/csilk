# csilk 项目完整规范文档

**版本**: v0.4.0  
**最后更新**: 2026-08-16  
**状态**: 活跃开发

---

## 1. 项目概览

### 1.1 定位
csilk 是一个轻量级、高性能的 C 语言 HTTP Web 框架，面向高并发、低延迟场景设计。

| 指标 | 目标值 |
|------|--------|
| 静态二进制大小 | ~150 KB |
| 内存占用 (10K keep-alive) | < 2 MB RSS |
| P99 延迟 | ≤ 5ms @ 10K QPS |
| 吞吐量 (16 核) | ~200K QPS (线性扩展) |

### 1.2 技术栈

| 层次 | 组件 | 说明 |
|------|------|------|
| I/O 后端 | libuv (默认) / io_uring (可选) | 事件循环抽象层 |
| HTTP 解析 | llhttp | 高性能 HTTP/1.x 解析器 |
| HTTP/2 | nghttp2 | ALPN 协商、多路复用 |
| JSON | cJSON / yyjson | 请求/响应序列化 |
| TLS | OpenSSL 1.1.1+ | TLS 1.3 强制要求 |
| 配置 | libyaml | YAML 格式服务器配置 |
| 存储 | SQLite3 | 嵌入式数据库驱动 |

### 1.3 项目统计

| 指标 | 数值 |
|------|------|
| C 源文件 | 304 (src) + 170 (tests) |
| 代码行数 | ~67,700 行 |
| CI 矩阵 | 2 OS × 2 构建类型 × 4 jobs = 8+ 任务 |
| 单元测试 | 168 个通过 |
| 集成测试 | 2 个通过 |
| 近 30 天提交 | 228 commits |

---

## 2. 架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Application Layer                            │
│  examples/  tests/  middleware/  protocols/  workflow/              │
├─────────────────────────────────────────────────────────────────────┤
│                        Core Framework                               │
│  src/core/{server, http, ctx, primitives, config, cache, plugin}    │
├─────────────────────────────────────────────────────────────────────┤
│                      I/O Backend Layer                              │
│  src/core/{uring, io}  [libuv → csilk_io_* 抽象]                    │
├─────────────────────────────────────────────────────────────────────┤
│                    Drivers & Messaging                              │
│  src/{drivers, messaging}  [DB, Vector, AI, MQ, Raft]              │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块依赖关系

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   server     │────▶│    http      │────▶│    ctx       │
└──────────────┘     └──────────────┘     └──────────────┘
       │                                       │
       ▼                                       ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   middleware │◀────│  primitives  │◀────│    io/       │
└──────────────┘     └──────────────┘     │  uring/      │
       │                                  └──────────────┘
       ▼
┌──────────────┐     ┌──────────────┐
│  workflow    │────▶│  messaging   │
└──────────────┘     └──────────────┘
       │
       ▼
┌──────────────┐
│   drivers    │
│  (DB/Vector) │
└──────────────┘
```

### 2.3 I/O 抽象层设计

`csilk_io_*` API 屏蔽底层实现差异，支持两种后端：

| API 类别 | 函数示例 | 用途 |
|----------|----------|------|
| Loop | `csilk_io_run()`, `csilk_io_loop_init()` | 事件循环管理 |
| Timer | `csilk_io_timer_start()`, `csilk_io_timer_stop()` | 定时器 |
| TCP | `csilk_io_tcp_open()`, `csilk_io_listen()` | TCP 连接 |
| Stream | `csilk_io_read_start()`, `csilk_io_accept()` | 流式 I/O |
| Async | `csilk_io_async_init()` | 跨线程通知 |
| Signal | `csilk_io_signal_start()` | 信号处理 |

**关键设计决策**:
- 所有 server core 代码使用 `csilk_io_*` 而非原始 `uv_*` 或 `pthread_*`
- `uv_barrier_t` 必须堆分配（跨线程使用）
- 工作线程的 `active_clients` 严格单线程 confined

---

## 3. 核心模块规范

### 3.1 Server 模块 (`src/core/server/`)

#### 3.1.1 服务器生命周期

```
csilk_server_new() → csilk_server_run() → [处理请求] → csilk_server_stop()
                                        ↓
                                   csilk_server_free()
```

| 阶段 | 关键操作 | 注意事项 |
|------|----------|----------|
| 创建 | 初始化 router、配置 worker pool | 必须传入已配置的 router |
| 运行 | 启动 worker 线程、绑定端口 | 阻塞直到 `csilk_server_stop()` |
| 停止 | 设置 stop_flag、drain 事件循环 | 需要等待 pending callbacks 完成 |
| 释放 | 关闭所有连接、销毁 worker | 先 stop 再 free |

#### 3.1.2 Worker 模型

```c
typedef struct csilk_worker_s {
    csilk_io_loop_t*  loop;           // 每个 worker 独立的 event loop
    csilk_io_tcp_t    acceptor;       // 监听 socket
    int               worker_id;      // worker 索引
    int               active_clients; // ⚠️ 仅本 worker 线程访问
    // ... 缓冲区池等
} csilk_worker_t;
```

**关键规则**:
- `active_clients` **严格单线程 confined**，不允许跨线程访问
- 跨线程操作必须使用 `csilk_dispatch(ctx, cb, arg)` 回调到 owning worker
- 每个 worker 维护独立的读缓冲区池（三层：4KB/16KB/64KB）

### 3.2 HTTP 模块 (`src/core/http/`)

| 文件 | 功能 |
|------|------|
| `http1_parse.c` | HTTP/1.x 请求解析（基于 llhttp） |
| `http1_response.c` | HTTP/1.x 响应构建 |
| `http1_zerocopy.c` | Zero-copy sendfile 集成 |
| `h2.c` | HTTP/2 协议处理（基于 nghttp2） |
| `h2_callbacks.c` | HTTP/2 帧回调处理 |
| `swar_http.c` | SWAR 并行 HTTP 解析优化 |
| `tls.c` | TLS/SSL 封装 |

**Zero-copy 设计**:
- 直接引用 TCP/SSL 接收缓冲区，避免 heap malloc/free
- 使用 `csilk_str_view_t` 表示 URL、headers、body
- 请求 arena 负责生命周期管理

### 3.3 Context 模块 (`src/core/ctx/`)

```c
typedef struct csilk_ctx_s {
    csilk_io_work_t*   work;        // 底层 IO 工作结构
    csilk_router_t*    router;      // 共享路由表
    csilk_server_t*    server;      // 服务器引用
    csilk_arena_t*     arena;       // 请求级内存池
    csilk_json_t*      json;        // JSON 处理上下文
    csilk_mq_t*        mq;          // 消息队列引用
    // ... 请求/响应数据
} csilk_ctx_t;
```

**关键 API**:

| 函数 | 用途 |
|------|------|
| `csilk_get_path_view()` | 获取请求路径 |
| `csilk_get_body_view()` | 获取请求 body |
| `csilk_get_headers()` | 获取 header map |
| `csilk_get_param_view()` | 获取 URL 参数 |
| `csilk_get_query_view()` | 获取 query string |
| `csilk_bind_json()` | 自动绑定 JSON 到结构体 |
| `csilk_set_ex()` | RAII 式存储（自动释放） |
| `csilk_ctx_defer()` | 延迟清理（类似 Go defer） |

**Arena 内存语义**:
- `csilk_arena_alloc()` → 返回未初始化内存（零开销）
- `csilk_arena_calloc()` → 返回零初始化内存
- 请求结束自动 reset arena，无需手动 free

### 3.4 Router 模块 (`src/core/primitives/router*.c`)

```
Trie Router + SIMD 加速
```

| 特性 | 实现 |
|------|------|
| 路径匹配 | Radix Trie (树形结构) |
| SIMD 加速 | AVX2/AVX-512 (x86_64), NEON (aarch64) |
| 性能 | ~50ns/route (x86_64), ~80ns/route (ARM) |
| 并发 | 只读多线程安全，写入需锁 |

### 3.5 Workflow 模块 (`src/workflow/`)

```
DAG 工作流引擎
```

| 节点类型 | 说明 |
|----------|------|
| `flakey` | 普通处理节点 |
| `ai` | AI LLM 调用节点 |
| `vector_search` | 向量相似度搜索 |
| `agent_react` | ReAct Agent 节点 |
| `agent_worker` | Worker Agent 节点 |
| `agent_hitl` | Human-in-the-loop 节点 |

**重试机制**:
- `csilk_wf_node_set_retry(node, max_attempts, delay_ms)`
- 支持超时、错误目标跳转
- 基于 io_uring timer 实现延迟回调

### 3.6 Messaging 模块 (`src/messaging/`)

```
内部事件总线 + Raft 共识
```

| 组件 | 文件 | 功能 |
|------|------|------|
| MQ 核心 | `mq_core.c` | 发布/订阅、路由 |
| MQ 调度 | `mq_dispatch.c` | 线程间消息分发 |
| MQ WAL | `mq_wal.c` | 持久化日志 |
| Raft 共识 | `raft_consensus.c` | 分布式状态机 |
| Raft RPC | `raft_rpc.c` | 节点间通信 |
| Raft WAL | `raft_wal.c` | Raft 日志持久化 |

---

## 4. 中间件规范

| 中间件 | 头文件 | 功能 |
|--------|--------|------|
| auth | `middleware/auth.c` | HTTP Basic/Digest Auth |
| cors | `middleware/cors.c` | CORS 策略 |
| csrf | `middleware/csrf.c` | CSRF Token 验证 |
| jwt | `middleware/jwt.c` | JWT 验证 (HS256/RS256) |
| ratelimit | `middleware/ratelimit.c` | 固定窗口限流 |
| sliding_ratelimit | `middleware/sliding_ratelimit.c` | 滑动窗口限流 |
| gzip | `middleware/gzip.c` | Gzip 压缩 |
| static | `middleware/static.c` | 静态文件服务 |
| sse | `middleware/sse.c` | Server-Sent Events |
| waf | `middleware/waf.c` | Web Application Firewall |
| xdp_waf | `middleware/xdp_waf.c` | eBPF XDP WAF |
| circuit_breaker | `middleware/circuit_breaker.c` | 熔断器 |
| otlp_trace | `middleware/otlp_trace.c` | OpenTelemetry 追踪 |
| otlp_exporter | `middleware/otlp_exporter.c` | OTLP JSON 导出 |
| grpc_gateway | `middleware/grpc_gateway.c` | HTTP/gRPC 转换 |
| validate | `middleware/validate.c` | 请求校验 |
| session | `middleware/session.c` | Session 管理 |
| request_id | `middleware/request_id.c` | 请求 ID 注入 |
| metrics | `middleware/metrics.c` | Prometheus Metrics |
| logger | `middleware/logger.c` | 结构化日志 |
| multipart | `middleware/multipart.c` | 文件上传解析 |

---

## 5. 构建系统规范

### 5.1 模块化库目标

| 目标别名 | 静态库 | 动态库 | 说明 |
|----------|--------|--------|------|
| `csilk::core` | `libcsilk-core.a` | `libcsilk-core.so` | 核心原语、arena、ctx |
| `csilk::http` | `libcsilk-http.a` | `libcsilk-http.so` | HTTP/1 服务器、中间件 |
| `csilk::tls` | `libcsilk-tls.a` | `libcsilk-tls.so` | TLS 1.3 加密引擎 |
| `csilk::http2` | `libcsilk-http2.a` | `libcsilk-http2.so` | HTTP/2 (nghttp2) |
| `csilk::db` | `libcsilk-db.a` | `libcsilk-db.so` | DB 抽象、SQLite、Vector |
| `csilk::ai` | `libcsilk-ai.a` | `libcsilk-ai.so` | AI LLM 客户端 |
| `csilk::mq` | `libcsilk-mq.a` | `libcsilk-mq.so` | 消息队列、PubSub、Raft |
| `csilk::workflow` | `libcsilk-workflow.a` | `libcsilk-workflow.so` | 工作流 DAG 调度器 |
| `csilk::csilk` | `libcsilk.a` | `libcsilk.so` | 完整 umbrella 库 |

### 5.2 CMake 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | Release | Debug/Release |
| `CSILK_USE_URING` | OFF | 启用 io_uring 后端 |
| `CSILK_BUILD_SHARED` | OFF | 构建共享库 |
| `USE_ASAN` | OFF | Address Sanitizer |
| `USE_TSAN` | OFF | Thread Sanitizer |
| `USE_COVERAGE` | OFF | 代码覆盖率 (需 gcc) |
| `ENABLE_OOM_TEST` | OFF | OOM 模拟测试 |
| `CSILK_ENABLE_NATIVE_ARCH` | OFF | -march=native |

### 5.3 构建命令

```bash
# 标准构建
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# io_uring 后端
cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Debug -DCSILK_USE_URING=ON
cmake --build build_uring -j$(nproc)

# ASAN
cmake -B build_asan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON
cmake --build build_asan -j$(nproc)
```

---

## 6. 测试策略

### 6.1 测试分类

| 类别 | 目录 | 数量 | 运行命令 |
|------|------|------|----------|
| 单元测试 | `tests/{app,core,crypto,drivers,...}` | 168 | `ctest -E test_integration` |
| 集成测试 | `tests/integration/` | 2 | `ctest -R test_integration` |
| Fuzz 测试 | `fuzz/` | 4 | 独立 CI job |
| Python 绑定 | `python/tests/` | - | 独立 CI job |

### 6.2 测试覆盖模块

```
tests/
├── app/           # 应用层测试
├── core/
│   ├── cache/     # MVCC 缓存
│   ├── config/    # 配置解析
│   ├── ctx/       # 上下文管理
│   ├── http/      # HTTP 解析
│   ├── io/        # I/O 抽象
│   ├── json/      # JSON 处理
│   ├── plugin/    # WASM 插件
│   ├── primitives/# 路由、query、response
│   └── server/    # 服务器生命周期
├── crypto/        # base64, sha1, bcrypt, uuid
├── drivers/
│   ├── db/        # SQLite, Redis
│   └── vector/    # HNSW, SIMD
├── integration/   # HTTP 集成测试
├── messaging/     # MQ, Raft
├── middleware/    # 所有中间件
├── protocols/     # WebSocket, SSE, HTTP/2
├── reflection/    # 反射绑定
├── security/      # bcrypt, JWT 安全
└── workflow/      # 工作流引擎
```

### 6.3 TEST_OOM 特殊说明

`-DENABLE_OOM_TEST=ON` 定义 `TEST_OOM`：
- 强制确定性盐值（bcrypt）
- 内存分配 fake（可控失败）
- **注意**: 断言 hash 相等性的测试必须用 `#ifdef TEST_OOM` 包裹

---

## 7. CI/CD 规范

### 7.1 CI 矩阵

| Job | OS | Build | 特殊标记 |
|-----|-----|-------|----------|
| Test | ubuntu-24.04 | Debug | ASAN |
| Test | ubuntu-24.04 | Release | - |
| Test | macos-14 | Debug | - |
| Test | macos-14 | Release | - |
| io_uring | ubuntu-24.04 | Release | native 后端 |
| ThreadSanitizer | ubuntu-24.04 | Debug | TSAN |
| Fuzz Testing | ubuntu-24.04 | Debug | 4 fuzzers |
| ARM64 Cross-Compile | linux | Release | aarch64 |
| Lint & Static | ubuntu-24.04 | - | clang-tidy |

### 7.2 已知问题与缓解

| 问题 | 影响 | 缓解措施 |
|------|------|----------|
| `test_workflow_retry` 在 Release+io_uring 下 hang | CI 失败 | 添加 loop limit + ctest timeout |
| `test_integration_ext` SIGPIPE | 偶发崩溃 | `signal(SIGPIPE, SIG_IGN)` |
| io_uring timer CQE 返回 -ECANCELED | 定时器不触发 | 接受 -ECANCELED 作为有效完成 |

---

## 8. 编码规范

### 8.1 Git 提交规范 (Gitmoji)

```
type(scope): emoji subject
```

| Type | Emoji | 说明 |
|------|-------|------|
| feat | ✨ | 新功能 |
| fix | 🐛 | Bug 修复 |
| docs | 📝 | 文档 |
| style | 🎨 | 代码格式 |
| refactor | ♻️ | 重构 |
| perf | ⚡ | 性能优化 |
| test | ✅ | 测试 |
| build | 📦 | 构建系统 |
| ci | 👷 | CI/CD |
| chore | 🧹 | 杂项 |

### 8.2 关键编码规则

1. **禁止在 server core 使用原始 `uv_*` 或 `pthread_*`**
   - 必须使用 `csilk_io_*`, `csilk_thread_*`, `csilk_barrier_*`
   
2. **`uv_barrier_t` 必须堆分配**
   ```c
   // ❌ 错误
   uv_barrier_t barrier;
   
   // ✅ 正确
   uv_barrier_t* barrier = calloc(1, sizeof(*barrier));
   ```

3. **Worker 本地数据不可跨线程访问**
   ```c
   // ❌ 错误：直接访问其他 worker 的 active_clients
   wp->active_clients++;
   
   // ✅ 正确：通过 dispatch 回调
   csilk_dispatch(ctx, ^(void* arg) {
       wp->active_clients++;
   }, NULL);
   ```

4. **Arena 分配语义**
   ```c
   // 需要零初始化
   void* p = csilk_arena_calloc(arena, size);
   
   // 不需要零初始化（零开销）
   void* p = csilk_arena_alloc(arena, size);
   ```

5. **RAII 式存储**
   ```c
   // ✅ 正确：自动释放
   csilk_set_ex(c, "key", ptr, destructor);
   
   // ❌ 错误：需要手动管理生命周期
   csilk_set(c, "key", ptr);
   ```

6. **Crypto 栈缓冲区**
   ```c
   uint8_t pwd_buf[72];
   memset(pwd_buf, 0, sizeof(pwd_buf));  // 必须
   memcpy(pwd_buf, password, len);
   ```

### 8.3 格式化要求

```bash
# 提交前必须运行
cmake --build build --target format
```

格式化目标：`src/*.c`, `src/*.h`, `include/*.h`, `tests/*`, `examples/*`

---

## 9. 外部依赖

| 依赖 | 版本要求 | 用途 | 获取方式 |
|------|----------|------|----------|
| libuv | ≥ 1.40 | 默认 I/O 后端 | pkg-config |
| liburing | ≥ 2.1 | io_uring 后端 | FetchContent |
| llhttp | ≥ 6.0 | HTTP 解析 | FetchContent |
| nghttp2 | ≥ 1.40 | HTTP/2 | FetchContent/pkg-config |
| cJSON | 内置 | JSON 处理 | 源码内置 |
| yyjson | ≥ 0.8 | 快速 JSON | FetchContent |
| OpenSSL | ≥ 1.1.1 | TLS, crypto | pkg-config |
| ZLIB | ≥ 1.2 | 压缩 | pkg-config |
| SQLite3 | ≥ 3.20 | 嵌入式数据库 | pkg-config |
| libyaml | ≥ 0.2 | YAML 配置 | pkg-config |
| curl | - | OTLP exporter | cmake find_package |

---

## 10. 已知技术债务

| 问题 | 优先级 | 说明 |
|------|--------|------|
| `test_workflow_retry` 在 Release 下偶发 hang | 高 | 需进一步调查 io_uring 定时器行为 |
| io_uring 定时器 CQE -ECANCELED 处理 | 中 | 已修复，但需回归测试 |
| 部分模块缺少设计文档 | 低 | 如 workflow, messaging |
| Python 绑定测试覆盖不足 | 低 | 建议补充 |
| macOS 上 libuv + io_uring 兼容性 | 中 | 仅 Linux 支持 io_uring |

---

## 11. 未来路线图

| 阶段 | 目标 |
|------|------|
| v0.5 | HTTP/3 (QUIC) 支持完善 |
| v0.5 | Vector DB 驱动扩展 (Pinecone, Weaviate) |
| v0.6 | WASM 插件系统增强 |
| v0.6 | gRPC 服务端完整支持 |
| v1.0 | 生产级稳定性认证 |

---

## 附录 A: API 速查

### A.1 服务器 API

```c
// 创建服务器
csilk_server_t* csilk_server_new(csilk_router_t* router);

// 运行（阻塞）
void csilk_server_run(csilk_server_t* server, unsigned short port);

// 停止
void csilk_server_stop(csilk_server_t* server);

// 释放
void csilk_server_free(csilk_server_t* server);
```

### A.2 路由 API

```c
csilk_router_t* csilk_router_new(void);
void csilk_router_add(csilk_router_t* r, const char* method, const char* path, 
                      csilk_handler_t handlers[], int count);
void csilk_router_free(csilk_router_t* r);
```

### A.3 上下文 API

```c
csilk_view_t csilk_get_path_view(csilk_ctx_t* c);
csilk_view_t csilk_get_body_view(csilk_ctx_t* c);
csilk_header_map_t* csilk_get_headers(csilk_ctx_t* c);
csilk_view_t csilk_get_param_view(csilk_ctx_t* c, const char* key);
csilk_view_t csilk_get_query_view(csilk_ctx_t* c, const char* key);
csilk_json_t* csilk_bind_json(csilk_ctx_t* c);
```

### A.4 Workflow API

```c
csilk_wf_t* csilk_wf_new(const char* name);
csilk_wf_node_t* csilk_wf_add(csilk_wf_t* wf, const char* id, 
                               csilk_wf_handler_t handler, void* user_data);
void csilk_wf_node_set_retry(csilk_wf_node_t* node, int max_retries, int delay_ms);
void csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, 
                  void (*callback)(csilk_data_t* result));
void csilk_wf_free(csilk_wf_t* wf);
```

---

*文档自动生成于 2026-08-16，基于项目源码分析*
