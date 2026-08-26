# 更新日志

本文档记录了本项目的所有重要变更。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
并 adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [Unreleased]

### 安全修复

**严重:**
- **CSRF Token 熵源**: 移除了 `/dev/urandom` 不可用时的 `rand_r()` 降级路径。现在直接返回错误而非使用弱伪随机数 (CWE-330)。
- **Session Cookie Secure 标志**: 为 Session cookie 添加了 `Secure` 属性，防止通过明文 HTTP 传输 (CWE-1004)。

**高危:**
- **CSRF Token Cookie Secure 标志**: 为 CSRF token cookie 添加 `Secure` 属性。
- **安全响应头**: 新增 `csilk_security_headers_middleware()` 中间件，设置 `X-Frame-Options`、`X-Content-Type-Options`、`Referrer-Policy` 响应头 (CWE-79, CWE-1021)。
- **Multipart 上传大小限制**: 新增 `CSILK_MAX_PART_SIZE` (10 MB) 常量防止 DoS 攻击 (CWE-434)。

**中危:**
- **OTLP Trace 随机数**: 使用 OpenSSL `RAND_bytes()` 替换 `rand()` 生成 span ID (CWE-330)。
- **XDP WAF atoi 验证**: 为 CIDR 前缀长度解析添加范围检查 (CWE-284)。
- **配置超时验证**: 为超时配置值添加范围验证 (CWE-284)。

### 新增功能
- **公开 Cipher API**: 新增 `csilk_symmetric_encrypt/decrypt` (AES-256-GCM)、`csilk_rsa_generate_keypair`、`csilk_rsa_encrypt/decrypt`、`csilk_rsa_sign/verify`，定义在 `<csilk/core/cipher.h>` 中 — 无需请求上下文即可使用。
- **公开 HTTP/2 API** (`csilk/http/h2.h`): 从内部 `src/core/http/h2.h` 提升到公开头文件；新增 `csilk_h2_init_session`、`csilk_h2_process_data` 等函数。
- **公开 Flame Graph API** (`csilk/util/flamegraph.h`): 从内部提升到公开头文件；新增 `csilk_flamegraph_start`、`csilk_flamegraph_stop` 等函数。

### 变更
- **Crypto 模块重构**: 将 711 行的 `src/crypto/crypto.c` 拆分为 `crypto.c` (SHA-256, HMAC, UUID, RNG, nonce，约 297 行) 和 `src/crypto/cipher_dispatch.c` (AES/RSA/JWT 分发，约 350 行)。将 `src/crypto/url.c` 移至 `src/core/primitives/url.c`。
- **头文件目录对齐**: 所有公开头文件现在镜像 src/ 模块布局。仅内部使用的头文件 (`header_map.h`、`query.h`、`lfqueue.h`) 保留在 `src/` 中。
- **测试数量**: 211 → 213 (新增 cipher 公开 API 测试：5 个测试函数)。

### 修复
- **clang-tidy**: 所有变更文件零警告。

---

## [0.5.2] - 2026-08-23

### 修复
- **JWT 中间件 NULL 密钥路径**: `csilk_jwt_middleware(c, NULL)` 不再静默返回无响应。现在发送 HTTP 500 `Internal Server Error` 并中止处理器链，与其他中间件 (ratelimit, csrf) 行为一致。新增 `test_jwt_middleware_null_key` 测试用例。
- **ASAN 内存泄漏**: 修复了 `test_hot_reload_null` (缺少 `csilk_router_free`)、`test_uring_fs` (过早 `csilk_io_loop_close` 导致的 use-after-free) 和 `csilk_io_fs_sendfile` 在 NULL 请求上的段错误。
- **Clang 构建兼容性**: 添加了定义在 `main()` 之后的测试函数的前向声明 — GCC 容忍此写法但 Clang 拒绝。

### 新增
- **SSE 集成测试** (`test_sse_integration`): 7 个测试用例覆盖 SSE 初始化、发送、关闭和通过实时 HTTP 服务器的头验证。
- **MCP stdio 测试** (`test_mcp_stdio`): 5 个测试用例覆盖 JSON-RPC initialize、tools/list、tools/call、未知方法和缺失参数 (通过 forked 子进程)。
- **Session 集成测试** (`test_session_integration`): 7 个测试用例覆盖 session start/get/set 生命周期。
- **Admin 集成测试** (`test_admin_integration`): 11 个测试用例覆盖 `/admin/stats`、`/admin/`、`/admin/topology` 和 404 路径。
- **中间件链集成测试** (`test_middleware_chain_integration`): 6 个测试用例验证中间件执行顺序。
- **OpenAPI 集成测试** (`test_openapi_integration`): 5 个测试用例用于 OpenAPI JSON 和 Swagger UI 端点。
- **ctx_json 单元测试** (`test_ctx_json`): 10 个测试用例用于 `csilk_bind_json`、`csilk_bind_json_err`、`csilk_get_cookie`、`csilk_bind_reflect`。
- **JSON mutate 边界情况测试**: 为 `test_json_mutate` 添加了 null-idoc 和 null-mdoc 守卫用例。
- **Request ID readiness 处理器测试**: 为 `csilk_ready_check_handler` 添加了空安全测试。

### 变更
- **重构 server_lifecycle.c**: 将 RCU 管理提取到 `server_rcu.c` (569 行)，驱动注入提取到 `server_driver.c` (59 行)。
- **添加 .gcovr 配置**: 从覆盖率报告中排除不可测试的文件 (`flamegraph.c`、`redis_storage.c`、`workflow_debug.c`、`uring_vector.c`)。

### 测试覆盖
- **总测试数**: 213 (211 单元 + 2 集成系列)
- **行覆盖率**: 66% (11,765/17,773 行)
- **sse.c**: 22% → 77% (SSE 集成测试)
- **hot_reload.c**: 5% → 66% (动态库重载测试)

---

## [0.5.1] - 2026-08-22

### 新增
- **形式化客户端生命周期验证与所有者归属**: 在 100,000 次重用迭代中形式化验证客户端生命周期状态机，证明 `client_destroy` 严格在所属 worker 循环上执行，非所有者线程入队带生成标签的回收任务 (`_csilk_client_recycle_dispatch_cb`)，且挂起的 I/O / 引用计数器从不溢出。
- **RCU / EBR 形式化验证与 512 读者扩展**: 新增包含 512 并发读者 (静态 256 + 动态溢出槽) 和 10,000 短生命周期线程的 RCU 生命周期压力套件，证明零动态槽泄漏、安全的 TID 重用和锁等待自由的读路径。在 `config_mutex` 下序列化路由器交换以确保单调纪元前进。
- **HTTP/2 流多路复用形式化生命周期**: 形式化验证流多路复用生命周期 (`stream_new`、`h2_stream_map` 查找/驱逐带 16 内联桶、`free_list` 回收、RST_STREAM、GOAWAY 和异步完成)，保证流回调从不访问回收的上下文。
- **PMU 引导的微优化**: 微优化关键路径 (`client_ref`/`client_unref`、`pending_io_inc`/`pending_io_dec`、RCU 嵌套深度跟踪、`g_dispatch_tls_registered` 分支缓存和 HeaderMap 快速位掩码短路)，在不损害正确性的情况下减少核心周期占用。
- **无等待 MPSC 队列加固**: 在 `csilk_lfq_dequeue()` 中添加空安全守卫和自动 worker 池队列初始化，确保跨任意线程拓扑的稳健多生产者分派。
- **有序拆卸序列**: 强制干净拆卸顺序 (`server_stop` → 排空活动客户端 → 排空定时器 → 排空分派队列 → join workers → 停止热重载 → EBR 宽限期 → 销毁路由器 → 关闭事件循环 → 释放池)。推迟 MQ 拆卸到 worker 线程 join 之后以防止 worker 退出时的异步崩溃。

### 修复
- **分派异步清理与循环排空**: 安全关闭 `wp->dispatch_async` 并在 `csilk_server_free()` 期间排空挂起的事件循环句柄，防止在默认 libuv 事件循环中悬挂句柄。

---

## [0.5.0] - 2026-08-22

### 新增
- **热重载互斥与安全临时文件**: 在 `hot_reload_ctx_t` 中添加 `csilk_mutex_t reload_mutex`，确保文件系统监听器去抖和手动 `csilk_dev_hot_reload_trigger()` 永不竞争或并发修改重载状态。强制执行安全的 `mkstemp(0600)` 并立即报告失败，实现完整的 OOM 回滚 (`dlclose`、`unlink`、`csilk_router_free`)。
- **自适应 io_uring 队列 sizing 与资源回退**: 用自适应回退 (1024 → 512 → 256 → 128 → 64) 替换硬编码的 4096 条目 ring 初始化，以及成比例的池容量，消除受限容器/VM 环境 (`RLIMIT_MEMLOCK`) 中的 `-ENOMEM` 错误。
- **6 层统一内存所有权分类**: 定义全面的 6 层内存所有权模型 (`csilk_ownership_t`: `BORROWED`、`ARENA`、`OWNED`/`HEAP`、`TRANSFER`、`POOL`、`TLS_CACHE`) 和字符串化器 `csilk_ownership_str()`，定义在 `<csilk/core/types.h>` 中。标准化容量感知的缓冲区清理和池回收在 `_csilk_ctx_cleanup()` 中，并统一响应体内存替换守卫 (`_csilk_free_response_body_if_needed()`)。
- **3 层 ABI 架构与严格不透明封装**: 强制实施严格的 `Public API → Opaque Handle → Internal Implementation` 架构，覆盖所有 52 个 `include/` 下的公开头文件:
  - 将 `csilk_router_t` 转换为 `<csilk/core/router.h>` 中的严格不透明句柄，将 `struct csilk_router_s` 和 trie 节点结构移至 `src/core/primitives/router_internal.h`。
  - 解耦 OpenSSL 从公开头文件: `<csilk/core/hash.h>` 现在将 `csilk_sha1_ctx` 和 `csilk_sha256_ctx` 定义为 64 位对齐的不透明内存缓冲区 (128 字节)，通过 `_Static_assert` 在编译时验证。
  - 解耦后端 I/O 句柄: 从 `<csilk/core/context.h>` 移除 `csilk/core/sys_io.h` 并将 `csilk_get_work_req` 移到 `src/core/ctx/ctx_internal.h`。
- **异步上下文安全与生成跟踪**: 添加活动引用计数器 (`_csilk_ctx_async_ref_incr` / `_csilk_ctx_async_ref_decr`)、单调递增的请求序列号 (`request_seq`) 和 UUID v4 请求标签 (`request_id`)，确保异步 worker/DB/MQ 回调不能触摸回收的上下文或导致 keep-alive 连接上的 use-after-free。
- **连接生命周期状态机**: 实现显式 9 状态连接生命周期 (`csilk_conn_state_t`: `INIT`、`ACCEPTED`、`TLS`、`READING`、`PROCESSING`、`WRITING`、`STREAMING`、`CLOSING`、`CLOSED`)，带有不变量转换检查 (`csilk_conn_set_state`、`csilk_conn_get_state`、`csilk_conn_state_str`)，防止 use-after-free、双重关闭、双重释放和异步写入/流式传输竞态条件。
- **I/O 与同步抽象层**: 标准化统一的跨后端 `csilk_io_*`、`csilk_thread_*` (`csilk_thread_create`、`csilk_thread_join`、`csilk_thread_self`、`csilk_thread_setaffinity`) 和 `csilk_barrier_*` (`csilk_barrier_init`、`csilk_barrier_wait`、`csilk_barrier_destroy`) API，定义在 `<csilk/core/sys_io.h>` 和 `<csilk/core/sync.h>` 中。

- **流式传输背压与水 marks 流量控制**: 在 HTTP/1.1 分块流式传输 (`csilk_response_write`)、SSE (`csilk_sse_send`) 和 WebSocket (`csilk_ws_send`) 上实现每连接出站队列背压。添加可配置的高水 marks (`write_high_water_mark`，默认 64KB)、低水 marks (`write_low_water_mark`，默认 16KB)、最大缓冲区限制 (`max_write_buffer_size`，默认 16MB) 和异步排空回调注册 (`csilk_on_drain` / `csilk_set_write_watermarks`)。
- **上下文存储析构函数支持**: 添加带 `csilk_destructor_t` 回调的 `csilk_set_ex()`，用于在上下文 arenas 重置时 RAII 清理堆值。JWT 中间件现在自动绑定 `csilk_json_free` 到 `jwt_payload`。
- **类型化零拷贝视图**: 添加 `csilk_view_t` (`const char* data; size_t len;`)，带有显式借用视图访问器 (`csilk_get_query_view`、`csilk_get_param_view`、`csilk_get_header_view`、`csilk_get_body_view`)，区分零拷贝解析器缓冲区与拥有的 NUL 终止 arena 字符串。
- **JWT 验证策略与选项**: 添加 `csilk_jwt_flags_t` (`CSILK_JWT_REQUIRE_EXP`、`CSILK_JWT_REQUIRE_NBF`、`CSILK_JWT_REQUIRE_IAT`)、`csilk_jwt_options_t` (算法、标志、时钟偏差余量)、`csilk_jwt_verify_options()` 和 `csilk_jwt_middleware_options()` 用于严格 JWT 声明强制执行。
- **Arena Calloc 与多层 TLS 缓存**: 添加 `csilk_arena_calloc()` 用于零初始化的 arena 分配，3 层线程局部块空闲列表 (4KB、16KB、64KB) 带 `max_total_bytes` 约束，以及 worker 线程退出清理 (`csilk_arena_flush_free_list`)。

- **Crypto 驱动可扩展性**: `csilk_crypto_driver_t` 现在支持 `sha1` (20 字节摘要) 和 `bcrypt_hash` (密码哈希) 回调，带有内部分发包装器 `_csilk_sha1()` 和 `_csilk_bcrypt_hash()` — 驱动可以替换内置的软件实现。
- **`csilk_cond_broadcast()`**: `<csilk/core/sync.h>` 中用于在条件变量上广播所有等待者的新函数。桥接 `pthread_cond_broadcast` 和 libuv (没有广播原语) 之间的差距。
- **Crypto 模块测试**: `tests/crypto/test_crypto.c` 中 SHA-256、HMAC-SHA256、Base64/Base64URL 往返、`csilk_crypto_fill_random`、`csilk_crypto_generate_nonce` 和 `csilk_url_decode` 边界情况的全面基于属性的测试。

### 变更
- **OpenSSL 支持的密码原语**: 用系统 OpenSSL 原语替换手写的 SHA-256 (`csilk_sha256_*`)、HMAC-SHA256 (`csilk_hmac_sha256`) 和 SHA-1 (`csilk_sha1_*`) 实现，并重新设计 `bcrypt` 使用 OpenSSL `RAND_bytes()` / `RAND_priv_bytes()`、常量时间 `CRYPTO_memcmp()`、通过 `OPENSSL_cleanse()` 的安全内存清零和可重入线程安全密码状态。
- **便携式 Release 二进制默认值**: 将 Release 构建默认更改为便携式二进制 (不带 `-march=native`)，防止在旧 CPU、Docker 容器和 CI 工件上的 `SIGILL` 崩溃。主机原生 CPU 指令集调优现在是明确 opt-in，通过 `-DCSILK_ENABLE_NATIVE_ARCH=ON` (在 benchmark 脚本和 CI benchmark 工作流中自动启用)。

- **服务器核心纯抽象**: 从 `src/core/server/` (`connection.c`、`server_lifecycle.c`、`server_shutdown.c`、`server_worker.c`) 消除所有直接 `uv_*` 引用，用 `csilk_io_*` 和 `csilk_thread_*`/`csilk_barrier_*` 替换。

- **io_uring 架构精简**: 消除并行重复的服务器状态机 (`uring_server.c`、`uring_connection.c`、`uring_event_loop.c`)，将驱动整合到 `src/core/uring/uring_io.c` 下，并在所有后端统一单轨道服务器生命周期执行。
- **路由器前缀 Trie 架构与回滚**: 对齐路由器文档以反映基于段的 prefix trie 架构，并修复方法不匹配或处理器失败时的通配符参数回溯。
- **处理器链边界安全**: 在 `csilk_next()` 中添加显式 `handler_count` 检查以保护损坏或未终止的处理器数组。
- **线程归属与分派**: 显式记录 worker 本地 `active_clients` 归属；跨线程工作必须使用 `csilk_dispatch()`。
- **Barrier 生命周期**: `src/core/server/server_lifecycle.c` 中的 `uv_barrier_t` 现在堆分配 (`calloc`) 以防止 worker 线程超出栈本地 barrier 时的 use-after-free。现在检查 `uv_barrier_init` 返回值。
- **线程抽象**: `src/core/uring/uring_thread_pool.c` 用 `<csilk/core/sync.h>` 中的 `csilk_mutex_t`/`csilk_cond_t` 替换原始 `pthread_mutex_t`/`pthread_cond_t` 以实现跨后端一致性。
- **头文件卫生**: 从 `include/csilk/core/internal.h` 移除 `messaging/mq_internal.h` 传递包含。需要 `_csilk_mq_new`/`_csilk_mq_free` 的文件现在显式包含 `messaging/mq_internal.h`。
- **代码清理**: 1200+ 处标准化 `nullptr` → `NULL` 以实现 C23 一致性。修复 `-Wcomment` (connection.c)、`-Wformat` (qdrant.c、workflow_dsl.c) 和 `-Wformat` (session.c strdup 空检查)。

### 修复
- **csilk_server_free 中的 Owned 事件循环清理**: 当 `server->loop_owned` 为 true 时添加 `csilk_io_loop_close` 和 `server->loop` 的内存释放，消除快速服务器实例化 across io_uring 文件描述符泄漏。
- **多 Worker Sendfile 与 Hook 同步**: 使用 `CSILK_HOOK_SERVER_START` 同步多 worker sendfile 操作，并添加超时/重试策略防止多 worker socket 绑定期间的死锁。
- **io_uring 后端取消与监听 Socket 安全**: 修复 `csilk_io_timer_stop()` 使用定向 `io_uring_prep_cancel64` 带指针和生成标签而不是宽泛取消，防止计时器停止无意中取消服务器监听 socket SQEs。
- **io_uring 异步与信号轮询通知可靠性**: 在触发回调之前为 `URING_OP_POLL_ASYNC` 和 `URING_OP_POLL_SIGNAL` 添加 `read() > 0` 验证以防止虚假执行。
- **双后端构建与字段隔离**: 在 `src/core/server/connection.c` 中将 io_uring 特定句柄字段 (`generation`、`fd`) 隔离在 `#ifdef CSILK_USE_URING` 下，并引入便携式 `reject_connection()` 助手确保干净编译和 100% 测试通过率 across libuv 和 io_uring 后端。
- **路由器匹配调试日志安全**: 在 `csilk_router_match_ctx()` 日志语句中添加防御性空检查 for `mh`，消除 `clang-tidy` 检查期间的 `clang-analyzer-core.NullDereference`。
- **多 Worker 启动 Barrier 死锁**: 消除多 worker 初始化期间的无限挂起和死锁，当 worker 分配 (`worker_data_t`) 或线程创建 (`csilk_thread_create`) 中途失败时，补偿 barrier 计数 for 未分派的 worker 并执行干净优雅中止。
- **动态 TCP 读取缓冲区**: 动态扩展 `read_buffers` (初始 16 槽加倍) 以防止需要 >16 TCP 读取的请求的数据丢弃。
- **原子最大连接数**: 将 `max_connections` 检查转换为原子 CAS 预留 (`_csilk_server_try_acquire_connection`) 和回滚以消除高并发 TOCTOU 竞态条件。
- **JWT 内存泄漏**: 通过 `csilk_set_ex()` 绑定自动析构函数以在上下文重置时释放 cJSON payload 堆分配。
- **uv_barrier_t UAF**: 修复多 worker 服务器启动中的 use-after-free，其中栈分配的 `uv_barrier_t` 被主线程销毁而 worker 线程仍持有其地址。Barrier 现在堆分配并在所有 workers join 后释放。
- **internal.h MQ 泄漏**: 从 `include/csilk/core/internal.h` 移除 `#include "messaging/mq_internal.h"` 以停止传递暴露 MQ 内部 (例如 `csilk_mq_t`) 到包括 umbrella 头在内的每个文件。

---

## [0.4.0] - 2026-08-13

### 变更
- **目录结构重组**: 将 `base64.c`、`sha1.c`、`url.c`、`uuid.c`、`crypto.c` (原 `utils.c`) 从 `src/core/server/` 移到新的 `src/crypto/` 模块；将 `bcrypt.c` 和 `blowfish_sboxes.h` 移到 `src/crypto/` (合并 `src/security/`)；将 `admin.c` 从 `src/core/config/` 移到 `src/app/`；重组测试从 `tests/data/` 到 `tests/security/` 和 `tests/drivers/db/`；移除冗余 `include/csilk/core/admin.h` re-export 包装；移除 `CSILK_DATA_SOURCES` CMake 变量 (内联到 `CSILK_DRIVER_SOURCES`)。

### 新增
- **嵌入式 SIMD 向量索引引擎**: 32 字节对齐 AVX2 SIMD 距离内核 (`csilk_simd_vector_cosine`、`csilk_simd_vector_l2`、`csilk_simd_vector_dot`) 和多层 HNSW skip-graph 索引引擎 (`csilk_hnsw_index_t`) 支持 $O(\log N)$ ANN 向量相似性搜索 (`csilk_vector_db_new_embedded`)。
- **eBPF XDP 动态 WAF 与 OTLP APM Dashboard**: BPF-Map 热重载动态 WAF 规则引擎 (`csilk_xdp_waf_add_ip_rule`)、W3C trace 上下文中间件带 2048-span ring buffer (`csilk_otlp_tracer_start_span`) 和单页嵌入式 Web APM Dashboard (`share/csilk/apm_ui.html`、`/admin/apm`)。

### 安全
- **敏感缓冲区清零**: 在 csrf、jwt、session 和 websocket 模块使用后清零敏感缓冲区以防止数据泄漏。
- **JWT 整数溢出守卫**: 添加 JWT 解析中 base64 长度计算的溢出保护。

### 修复
- **bcrypt 空密码验证失败**: 修复 bcrypt 实现中的三个 bug 导致 `csilk_bcrypt_verify` 对空密码始终返回 -1: (1) `CSILK_BCRYPT_CIPHER_OUT` 是 23 而非 24 — 24 字节编码为 32 base64 字符，但 verify 只读取 31，丢弃最后一个字节；(2) `datal`/`datar` 在 Eksblowfish P-array 密钥循环 (Step 2) 之前未零初始化，导致 salt XOR 针对空密码的栈垃圾；(3) `pwd_buf` 在 `memcpy` 之前未 `memset` 零初始化，当 `len == 0` 时留下未初始化内存。将 `CSILK_BCRYPT_HASH_LEN` 从 61 更新为 62 以匹配修正后的哈希格式 (`$2a$XX$` + 22 salt + 32 checksum + NUL)。
- **clang-tidy 误报**: 抑制 `blowfish_encipher` 中 `XL ^= p[i]` 的 `clang-analyzer-core.uninitialized.Assign` — 分析器无法跟踪通过数组指针参数；代码正确。
- **编译警告**: 修复 `-Wcomment` (connection.c 块注释内的无效 `/*`)、qdrant.c 中的 `-Wformat` (使用 `%lld` for `int64_t`) 和 workflow_dsl.c (移除 snprintf 的死 NULL 参数)，应用 clang-format to bcrypt 签名 in crypto.h 和 crypto_dispatch.h。
- **Python wheel 打包**: 移除 `setup.py` 中的嵌套 `csilk/` 子目录路径；添加 CMakeLists.txt 中的 `if(NOT DEFINED)` 守卫以保留 setuptools 的 `-D` 值。
- **macOS rpath**: 为 delocate-wheel 兼容性在共享库 rpath 上设置 `@loader_path`。
- **路由宏安全**: 将路由宏包装在 `do { } while(0)` 中以在控制流语句中安全使用。
- **CI 兼容性**:  bump upload/download-artifact to v6 for Node 24 support，无样本时跳过 FlameGraph 上传，修复 benchmark-results 上传路径。
- **macOS 兼容性**: 为 macOS 构建添加便携式 `explicit_bzero` shim。

### 变更
- **头文件守卫现代化**: 用 `#pragma once` 替换所有 38 个公开头文件的 `#ifndef`/`#define` 头文件守卫。
- **API 文档**: 为 middleware、server 和 group 头文件中未记录的公共 API 函数添加 Doxygen 文档。
- **tag-release.sh**: 扩展到覆盖所有版本位置 — `src/` `.c` `@version`、`python/csilk/_version.py`、`cmake/ports/csilk/vcpkg.json`、`vX.Y.Z+` 文档头、`| Version: X.Y.Z` 元数据、ASCII 图版本和 `version: X.Y.Z` 代码块引用。

---

## [0.3.0] - 2026-06-27

### 新增
- **io_uring 后端 (仅 Linux，可选)**: 使用 `CSILK_USE_URING=ON` 在编译时完整的事件循环、accept、read、write 和 timer 实现。带自动回退的 Square-submission-polling (SQPOLL)。每个 worker 线程池带无锁分派队列。所有 122 测试通过。
- **文档**: 使用全面的 io_uring 后端覆盖更新所有文档 (架构、构建指南、测试指南、部署、性能调优、故障排除、设计)。
- **零拷贝 HTTP 解析** — 集成 C23 风格字符串视图 (`csilk_str_view_t`) 用于 HTTP 头、URL 和 body，直接引用网络接收缓冲区以消除堆 malloc/free 开销。
- **深层 struct 释放** — 添加 `csilk_struct_free_reflect` 以递归释放反射引擎内的嵌套 struct 指针。
- **静态循环引用检测** — 添加编译/启动时 DFS 图环检测算法以验证注册的反射类型并防止递归栈溢出。
- **CI 模糊测试**: 重新启用模糊测试作业 (期望 Ubuntu 24.04 上 clang-19 可用在 2026 年 6 月)。
- **扩展测试覆盖**: WAF (4→9)、Session (5→8)、Recovery (1→4)、CSRF (3→7)、Workflow 生命周期 (1→3)。
- **零拷贝分块写入**: `_csilk_send_data_owned()` 消除分块传输编码路径中的双重分配/拷贝。
- **ABI 不透明类型转换**: 将内部 struct 定义 (`csilk_ctx_s`、`csilk_server_s`) 从 `include/csilk/core/` 移到 `src/core/`。所有非框架代码现在 exclusively 通过公共访问器 API 访问上下文状态。
- **延迟清理 API** (`csilk_ctx_defer` / `csilk_ctx_defer_free`): Panic 安全资源管理。当 `csilk_panic()` 设置 `panicked=1` 时，延迟回调按 LIFO 顺序运行以释放堆分配、文件描述符和 mutex 锁然后 recovery 中间件发送 500 响应。
- **SIMD 加速路由器**: x86_64 上的 AVX2 路径匹配和 aarch64 上的 ARM NEON。CMake 自动检测带 `-mavx2` 标志。
- **无锁 per-worker 连接池**: 用 per-worker 无锁池替换 mutex 基础池以实现多核扩展。
- **macOS 14 ARM64 CI 支持**: 重新启用 macOS 到 CI 矩阵带 `fdatasync`→`fsync` 和 `SOCK_NONBLOCK` 回退。
- **实时 CPU 火焰图**: admin dashboard 中的 backtrace 采样和火焰图渲染。
- **TypeScript/Python SDK 生成**: 从 OpenAPI 规范自动生成 API 客户端。
- **动态 AI 工具发现**: agentic workflows 的 MCP-like 工具发现 API。
- **常量时间 JWT 签名比较**: 用常量时间比较替换 `strcmp`。
- **Python scaffold 工具**: 将 `csilkskel` 从 C 重写为交互式 Python 工具。
- **热重载支持**: `csilk_server_set_router` 用于运行时路由器替换。
- **HTTP/2 Phase 1 — Session scaffolding**: TLS ALPN 协商 (`h2` vs `http/1.1`)、nghttp2 session 初始化、`csilk_h2.h` 公共 API 带 `csilk_h2_init_session`、`csilk_h2_process_data`、`csilk_h2_get_or_create_stream`、`csilk_h2_free_streams` 和 `send_callback` 用于帧序列化。
- **HTTP/2 Phase 2 — 请求分派与响应**: 提取 `_csilk_dispatch_request` 用于 HTTP/1.1 和 HTTP/2 的统一路由。实现 nghttp2 回调 (`on_header_callback` 用于 pseudo-header + 常规头解析、`on_frame_recv_callback` 用于 END_STREAM 分派、`on_data_chunk_recv_callback` 用于 body 累积、`on_stream_close_callback` 用于上下文清理)。添加 `csilk_h2_send_response` 带 `body_read_callback` 数据提供者用于流式响应 body。
- **`test_h2` 测试套件**: 注册到 `cmake/tests.cmake`。
- **C23 语言标准**: 从 C11 升级到 C23 (`CMAKE_C_STANDARD 23`)。将 `#define` 常量转换为 `static constexpr` 以实现类型安全编译时值。移除 6 个 `#include <stdbool.h>` 行 (现在是 C23 关键字)。
- **Form URL-encoded 解析器**: 添加 `csilk_parse_form_urlencoded()` 和 `csilk_get_form_field()` 用于 `application/x-www-form-urlencoded` body 解析 (P5-1)。
- **Session 支持**: 带 `csilk_session_init/start/set/get/destroy` API 的基于 cookie 的内存 session 管理 (P5-2)。
- **HTTP Range 请求**: Static file 中间件现在支持 `Range` 头带 206 Partial Content 响应 (P5-3)。
- **请求验证中间件**: `csilk_validate()` 带 REQUIRED/INT/STRING/EMAIL 标志和 min/max 范围验证 (P5-4)。
- **连接对象池**: 通过空闲列表重用 `csilk_client_t` 对象以减少分配开销 (P3-5)。
- **URL 解码**: 实现 `csilk_url_decode()` 用于百分号解码查询参数。
- **SHA1/Base64 已知答案测试**: 14 个测试用例覆盖 RFC 3174 和 RFC 4648 向量。
- **WebSocket 集成测试**: 验证 101 Switching Protocols + `Sec-WebSocket-Accept` 头。
- **流式响应集成测试**: 验证带 `csilk_response_write/end` 的分块编码。
- **重定向测试**: 增强带 `csilk_redirect_simple`、301/302/307 状态码、空安全边界情况。

### 变更
- **原子 builtins 标准化** — 用标准 C11 `<stdatomic.h>` APIs 替换所有遗留编译器相关 GCC `__sync_*` atomics。
- **多 worker 循环安全** — 移除硬编码的 `uv_default_loop()` 引用，动态解析活动 worker 线程的事件循环以防止多 worker 数据竞争。
- **HTTPS 读取路径优化** — SSL 读取缓冲区现在从连接 arena 分配而不是栈，保持解密数据对零拷贝字符串视图安全。
- **Arena 安全**: 在 `csilk_arena_alloc` 中添加溢出守卫和零大小哨兵处理。
- **Middleware middleware**: 将 WAF (Web Application Firewall) 添加到 15 个内置中间件。
- **Admin storage limit 测试**: 修复 `test_admin` storage 溢出以存储非空值。
- **`_csilk_trigger_hooks`**: 设为非静态并在 `server_internal.h` 中声明 so the H2 module can fire lifecycle hooks。
- **`pool_put`**: 现在调用 `csilk_h2_free_streams` 在返回 client 到空闲池之前清理任何 H2 stream 上下文。
- **版本 bump**: 0.2.5 → 0.3.0 across all 18 version references。
- **常量迁移**: `CSILK_DEFAULT_*` (5)、`CSILK_MAX_PARAMS`、`CSILK_MAX_STORAGE`、`CSILK_MAX_CHILDREN`、`MAX_REG_STRUCTS`、`MAX_IP_ENTRIES`、`WINDOW_SIZE` 转换为 `static constexpr` 并移到适当的头文件。
- **连接池**: Pool size of 32 clients；pool 在 `csilk_server_free` 中排空。
- **流式响应**: `csilk_response_write/end` 现在设置 `is_async` 标志以防止双重写入；分块头尊重客户端 `Connection: close` 头。
- **Static Middleware**: 在所有 static 响应上添加 `Accept-Ranges: bytes` 头。
- **流式清理**: Terminal chunk 写入回调关闭连接而不是留给 timer (修复 use-after-free)。
- **Header Location**: `context_internal.h` 从 `src/core/` 移到 `include/`；`src/core` include 路径从 CMakeLists.txt 移除。
- **Doxygen 文档**: 使用 `@brief`、`@param`、`@return`、`@note` 注释完成所有 37 源/头文件的完整 Doxygen 注释。

### 修复
- **io_uring SQE starvation**: `csilk_client_write` 在 io_uring Submission Queue ring 满时可能静默丢弃响应。添加带退避的重试循环。
- **on_write_done 中的 stale keep_alive**: llhttp 9.3.1 在 `on_message_complete` 返回后清除 `F_CONNECTION_CLOSE`，导致 `llhttp_should_keep_alive()` 在写入完成回调中返回错误值。在 `_csilk_send_response` 中计算时缓存决定在 `client->keep_alive`。
- **零拷贝 form body 解析**: 修复 `csilk_parse_form_urlencoded` 使用显式 body 长度 (`csilk_arena_strndup` 而非 `csilk_arena_strdup`) 当零拷贝 HTTP body 引用 llhttp 的 TCP 缓冲区时该缓冲区在 body 边界不 null 终止。
- **ASan 泄漏**: 解决新测试和 Doxyfile 生成中的内存泄漏。
- **macOS 兼容性**: `fdatasync` → `fsync`、`SOCK_NONBLOCK` 处理。
- **CI ASan suppression**: 为 macOS false positives 添加 suppression。
- **Arena TLS free list 泄漏**: 添加 `csilk_arena_flush_free_list()` 在服务器 free 时调用以防止 server 在非主线程上运行时的 ASAN 检测泄漏。
- **MQ realloc 溢出**: 在 monitor 数组、global middleware 数组和 per-topic handler 数组增长路径中添加整数溢出守卫和 NULL 检查。
- **`csilk_db_query_param_json` 中的 SQL 注入**: 添加标准 SQL 单引号加倍转义。
- **HTTP 解析器内存泄漏**: `on_url` max URL 超出、`on_header_value` max size 超出 / buffer grow 失败现在释放 `current_url`、`current_header_field` 和 `current_header_value`。
- **app.c server 错误泄漏**: `csilk_server_new(NULL)` 在 `csilk_router_new()` 失败时成功；添加失败路径的 `csilk_server_free()`。
- **hot_reload.c 资源泄漏**: `dlclose`/`FreeLibrary` 在 `dlsym`/`GetProcAddress` 或 init 函数失败时未调用。
- **WAF null context segfault**: `csilk_waf_middleware(nullptr)` 在 `csilk_next(nullptr)` 未阻止路径上崩溃。
- **4 const-qualifier 警告**: `bounded_buf.c` 返回类型和 `static.c` C23 `strchr` overload。
- **GCC builtin atomics**: `perm.c` `__sync_val_compare_and_swap` → C11 `atomic_compare_exchange_strong`。
- 修复 `csilk_parse_form_urlencoded` Content-Type 检查逻辑 (严格 `application/x-www-form-urlencoded` 检查)。
- 修复 static 中间件中的内存泄漏: `body_is_managed = 1` for full file buffer ensures cleanup。
- 修复 `csilk_ctx_cleanup` + timer 交互在流式响应生命周期中。
- 修复 server.c 中 3 个 `csilK_` 拼写错误 (pool_get/pool_put parameter types) 和 session.c (typedef)。

---

## [0.2.5] - 2026-05-29

### 修复
- **多 worker 模式中的 client pool 数据竞争**: `pool_get`/`pool_put` 访问 `client_pool` 和 `client_pool_count` 无同步。在多 worker 模式下，`on_new_connection` 在任何事件循环线程上运行，导致两个线程获取相同的 client 对象。这触发 libuv 断言崩溃: `uv_accept: Assertion 'server->loop == client->loop' failed`。添加 `pool_mutex` 保护所有 pool 操作。

---

## [0.2.4] - 2026-05-28

### 新增
- **Redis 数据库驱动**: 新 `src/drivers/redis.c` 驱动使用 hiredis。支持带密码认证和 DB 索引选择的连接池。映射 Redis 回复类型到表结果: GET→1 行、HGETALL→field/value 对、KEYS/LRANGE→N 行平铺数组。通过 MULTI/EXEC/DISCARD 的事务。

---

## [0.2.3] - 2026-05-28

### 新增
- **统一 Admin Dashboard**: `/admin` 的基于 Web 的实时监控 dashboard 带 HTTP metrics、workflow 执行图、MQ queue 状态、DB pool telemetry、AI model 调用跟踪和 process metrics。提供带 WebSocket 实时事件的 `admin_ui.html` SPA。
- **MongoDB 数据库驱动**: 新 `src/drivers/mongodb.c` 驱动使用 libmongoc。支持连接池和统一 DB 查询接口。
- **MQ 消息状态监控**: 实时 MQ 事件、深度跟踪和 admin dashboard 集成的 JSON stats 端点。
- **全局 AI 遥测**: `src/ai/ai.c` 现在跟踪 model 调用、token 计数和 latency 供 admin dashboard 消费。
- **全局 DB 遥测**: `src/data/db.c` 跟踪所有数据库驱动的 pool size、active connections 和 query latency。

### 修复
- **test_workflow_monitor SEGFAULT**: 修复由 `calloc(1, 1024)` 引起的 heap-buffer-overflow — `csilk_ctx_t` 是 2944 字节，分配的 buffer 太小。在 ASan 下这在每次 CI 运行触发 SEGFAULT。
- **scaffold `csilk_perm_auto_middleware_passthrough`**: 替换为现有 `csilk_perm_auto_middleware` — 前者不存在，导致核心 API + perm 模式的编译失败。
- **MQ recovery regression**: 修复连接断开后的消息队列恢复。
- **Admin struct privacy**: 解析 admin 模块中 `csilk_ctx_t` 的不完整类型。
- **Mermaid syntax**: 修复 version 10+ 引用 for workflow Mermaid 可视化。
- **test_timeout flakiness**: 通过添加服务器就绪同步修复端口冲突。

### 变更
- **Header relocation**: `workflow_wal.h` 从 `src/app/` 移到 `include/csilk/app/` 以保持所有头文件在 `include/` 下。
- **Admin scaffold**: `csilkskel` 现在默认包含 admin dashboard 设置。
- **版本 bump**: 0.2.1 → 0.2.3

---

## [0.2.2] - 2026-05-27

### 新增
- **对称/非对称 Cipher 驱动**: 新 `csilk_cipher_driver_t` 接口支持 AES-256-GCM 和 RSA-2048。通过 `csilk_server_set_cipher_driver()` 注册 — 传 NULL 恢复默认。

---

*此 changelog 遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 格式并 adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。*

---

## [0.2.1] - 2026-05-25

### 新增
- **Form URL-encoded 解析器**: 新增 `csilk_parse_form_urlencoded()` 和 `csilk_get_form_field()` 用于 `application/x-www-form-urlencoded` body 解析 (P5-1)。
- **Session 支持**: 带 `csilk_session_init/start/set/get/destroy` API 的基于 cookie 的内存 session 管理 (P5-2)。
- **HTTP Range 请求**: Static file 中间件现在支持 `Range` 头带 206 Partial Content 响应 (P5-3)。
- **请求验证中间件**: `csilk_validate()` 带 REQUIRED/INT/STRING/EMAIL 标志和 min/max 范围验证 (P5-4)。
- **连接对象池**: 通过空闲列表重用 `csilk_client_t` 对象以减少分配开销 (P3-5)。
- **URL 解码**: 实现 `csilk_url_decode()` 用于百分号解码查询参数。
- **SHA1/Base64 已知答案测试**: 14 个测试用例覆盖 RFC 3174 和 RFC 4648 向量。
- **WebSocket 集成测试**: 验证 101 Switching Protocols + `Sec-WebSocket-Accept` 头。
- **流式响应集成测试**: 验证带 `csilk_response_write/end` 的分块编码。
- **重定向测试**: 增强带 `csilk_redirect_simple`、301/302/307 状态码、空安全边界情况。

### 变更
- **连接池**: Pool size of 32 clients；pool 在 `csilk_server_free` 中排空。
- **流式响应**: `csilk_response_write/end` 现在设置 `is_async` 标志以防止双重写入；分块头尊重客户端 `Connection: close` 头。
- **Static Middleware**: 在所有 static 响应上添加 `Accept-Ranges: bytes` 头。
- **流式清理**: Terminal chunk 写入回调关闭连接而不是留给 timer (修复 use-after-free)。
- **Header Location**: `context_internal.h` 从 `src/core/` 移到 `include/`；`src/core` include 路径从 CMakeLists.txt 移除。
- **Doxygen 文档**: 使用 `@brief`、`@param`、`@return`、`@note` 注释完成所有 37 源/头文件的完整 Doxygen 注释。

### 修复
- 修复 `csilk_parse_form_urlencoded` Content-Type 检查逻辑 (严格 `application/x-www-form-urlencoded` 检查)。
- 修复 static 中间件中的内存泄漏: `body_is_managed = 1` for full file buffer ensures cleanup。
- 修复 `csilk_ctx_cleanup` + timer 交互在流式响应生命周期中。
- 修复 server.c 中 3 个 `csilK_` 拼写错误 (pool_get/pool_put parameter types) 和 session.c (typedef)。

---

## [0.2.0] - 2026-05-23

---

## [0.1.0] - 2026-05-15

### 新增
- 首次发布，包含核心路由、中间件和服务器实现。
- 支持 JSON (cJSON)、WebSocket 和 YAML 配置。
- 内置中间件: Logger、Recovery、Auth、CORS、CSRF、Rate Limiting、Static Files。
- 全面的 Doxygen 文档。

---

*此 changelog 遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 格式并 adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。*
