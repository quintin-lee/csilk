# 更新日志

本文档记录了本项目的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
本项目遵循 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [Unreleased]

### 安全修复

**严重 (Critical):**
- **Auth 中间件请求管道中断 (CWE-285)**: 修复鉴权成功后未调用 `csilk_next(c)` 导致请求被静默阻断的问题。
- **Session 密钥提前清零 Bug (CWE-665)**: 纠正敏感键清零时机，避免在缓存检索前清空 Session ID。
- **SQL 注入漏洞防御 (CWE-89)**: 在数据库驱动层统一增加参数化 DML 执行接口 `csilk_db_exec_param()`。
- **JWT 算法混淆漏洞 (CWE-327)**: 强制解码并校验 JWT Header `alg` 字段与服务端期望算法一致，彻底杜绝 `alg: "none"` 及 RSA/HMAC 混淆攻击。
- **WebSocket 帧 64 位整型溢出 (CWE-190, CWE-122)**: 增加载荷长度 64 MB 上限（`CSILK_WS_MAX_FRAME_PAYLOAD`）与整型防溢出检查。
- **AI 驱动 Double Free (CWE-415)**: 修复 `openai.c` 在无效 URL 协议错误分支中的重复 `csilk_json_free()`。

**高危 (High):**
- **Base64URL 解码表初始化 (CWE-20)**: 显式填满 256 元素，防止非 ASCII 字节解码为 0 (`'A'`)。
- **开放重定向与各类伪协议绕过 (CWE-601)**: 增强 `csilk_redirect`，拦截 `//`、大小写混淆及 `javascript:/data:` 等伪协议。
- **CSRF 恒定时间比对与 Cookie 标志 (CWE-208, CWE-352)**: 使用 `CRYPTO_memcmp()` 防止时序攻击，并将 CSRF Cookie 设置为 `http_only = 0` 以适配单页应用。
- **Multipart 二进制上传截断 (CWE-125)**: 替换 `strstr` 为二进制安全 `_csilk_memmem`，安全支持包含 `\0` 字节的文件上传。
- **HTTP 响应头 CRLF 注入 / 响应拆分 (CWE-113)**: 在响应头写入底层自动过滤 `\r` 和 `\n` 控制字符。
- **MQ WAL 恢复帧长度整型溢出 (CWE-190, CWE-122)**: WAL 恢复增加 `topic_len`（64 KB）和 `payload_len`（64 MB）上限检查。

**中危与低危 (Medium & Low):**
- **Cookie SameSite 属性支持 (CWE-352)**: 增加 `csilk_set_cookie_ex()`，支持 `SameSite=Strict/Lax/None`。
- **WAF 请求体恶意载荷扫描 (CWE-693)**: 中间件增加对 POST JSON 和 Form 请求体的 SQL 注入、XSS 和路径穿越扫描。
- **MQ WAL 任务分配 OOM 空指针防御 (CWE-476)**: 为 `_mq_append_wal` 中的 `strdup` 和 `malloc` 补充严格 NULL 检查。
- **静态资源 Range 头部原地内存修改 (CWE-704)**: 移除对 `Range` 请求头的 `*dash = '\0'` 原地修改操作。
- **静态文件路径穿越 Windows 分隔符增强 (CWE-22)**: 补充对 `\` 反斜杠路径穿越序列的支持。
- **gRPC 网关 4KB 栈溢出与截断 (CWE-400)**: 改用 Arena 动态分配转码载荷，支持任意大小消息。
- **URL 百分号解码符号性防御 (CWE-20)**: `isxdigit()` 参数统一添加 `(unsigned char)` 转换。
- **Nonce 生成 32 位移位越界 (CWE-330)**: 将移位宽度限定在 32 位以内。
- **Blowfish 跨端序移植性**: 显式使用位移提取字节，保障大小端架构一致性。

### 新增
- **公开 Cipher API**: 在 `<csilk/core/cipher.h>` 中新增 `csilk_symmetric_encrypt/decrypt`（AES-256-GCM）、`csilk_rsa_generate_keypair`、`csilk_rsa_encrypt/decrypt`、`csilk_rsa_sign/verify`，支持脱离请求上下文的独立加解密操作。
- **公开 HTTP/2 API**（`csilk/http/h2.h`）：由内部 `src/core/http/h2.h` 提升为公开头文件 `include/csilk/http/h2.h`，现包含 `csilk_h2_init_session`、`csilk_h2_process_data`、`csilk_h2_get_or_create_stream`、`csilk_h2_free_streams`、`csilk_h2_remove_stream`、`csilk_h2_send_response`、`csilk_h2_submit_push`。
- **公开火焰图 API**（`csilk/util/flamegraph.h`）：由内部 `src/util/flamegraph.h` 提升为公开头文件 `include/csilk/util/flamegraph.h`，现包含 `csilk_flamegraph_start`、`csilk_flamegraph_stop`、`csilk_flamegraph_is_running`。

### 变更
- **Crypto 模块重构**: 将 711 行的 `src/crypto/crypto.c` 拆分为 `crypto.c`（基础原语：SHA-256、HMAC、UUID、RNG、nonce，约 297 行）与 `src/crypto/cipher_dispatch.c`（密码调度：AES/RSA/JWT，约 350 行）。将 `src/crypto/url.c` 移动至 `src/core/primitives/url.c`（作为 HTTP 解析工具，非密码学范畴）。
- **头文件目录对齐**: 所有公共头文件现已镜像 `src/` 模块布局。仅内部使用的头文件（`header_map.h`、`query.h`、`lfqueue.h`）仍仅保留在 `src/` 中。
- **测试数量**: 211 → 213（新增 Cipher 公开 API 测试：5 个测试函数）。

### 修复
- **clang-tidy**: 所有变更文件零警告。

## [0.5.2] - 2026-08-23

### 修复
- **JWT 中间件 NULL Key 路径**: `csilk_jwt_middleware(c, NULL)` 不再静默返回而不发送响应，现发送 HTTP 500 `Internal Server Error` 并中止处理器链，与 ratelimit、csrf 等其他中间件行为一致。新增 `test_jwt_middleware_null_key` 测试用例。
- **ASAN 内存泄漏**: 修复 `test_hot_reload_null`（缺失 `csilk_router_free`）、`test_uring_fs`（过早调用 `csilk_io_loop_close` 导致 use-after-free）以及 `csilk_io_fs_sendfile` 在 NULL 请求下的段错误等堆泄漏问题。
- **Clang 构建兼容性**: 为 `main()` 之后定义的测试函数添加前向声明——GCC 可容忍此写法，但 Clang 会报错。

### 新增
- **SSE 集成测试**（`test_sse_integration`）：7 个测试用例，覆盖通过真实 HTTP 服务器进行的 SSE 初始化、发送、关闭及响应头校验。
- **MCP stdio 测试**（`test_mcp_stdio`）：5 个测试用例，通过 fork 子进程覆盖 JSON-RPC 的 initialize、tools/list、tools/call、未知方法以及缺失参数。
- **Session 集成测试**（`test_session_integration`）：7 个测试用例，覆盖 session 的 start/get/set 生命周期。
- **Admin 集成测试**（`test_admin_integration`）：11 个测试用例，覆盖 `/admin/stats`、`/admin/`、`/admin/topology` 及 404 路径。
- **中间件链集成测试**（`test_middleware_chain_integration`）：6 个测试用例，验证中间件执行顺序。
- **OpenAPI 集成测试**（`test_openapi_integration`）：5 个测试用例，覆盖 OpenAPI JSON 与 Swagger UI 端点。
- **ctx_json 单元测试**（`test_ctx_json`）：10 个测试用例，覆盖 `csilk_bind_json`、`csilk_bind_json_err`、`csilk_get_cookie`、`csilk_bind_reflect`。
- **JSON mutate 边界测试**: 为 `test_json_mutate` 新增 null-idoc 与 null-mdoc 守卫用例。
- **请求 ID 就绪处理器测试**: 为 `csilk_ready_check_handler` 新增空安全测试。

### 变更
- **重构 server_lifecycle.c**: 将 RCU 管理抽取为 `server_rcu.c`（569 行），将驱动注入抽取为 `server_driver.c`（59 行）。
- **新增 .gcovr 配置**: 将不可测试文件（`flamegraph.c`、`redis_storage.c`、`workflow_debug.c`、`uring_vector.c`）排除在覆盖率报告之外。

### 测试覆盖率
- **测试总数**: 213（211 个单元测试 + 2 个集成测试族）
- **行覆盖率**: 66%（11,765/17,773 行）
- **sse.c**: 22% → 77%（SSE 集成测试）
- **hot_reload.c**: 5% → 66%（动态库重载测试）

## [0.5.1] - 2026-08-22

### 新增
- **Client 连接生命周期形式化审计与 Worker 严格所有权归属**：在 100,000 次连接高频复用与 16-Worker 极端压力下完成形式化生命周期证明，确立 `client_destroy` 永远且仅在 Owner Worker 事件循环线程执行；非 Owner 线程只向目标 Worker 投递携带 `generation` 代数标记的回收任务（`_csilk_client_recycle_dispatch_cb`），彻底杜绝跨线程操作陈旧/已复用连接的 ABA 与 UAF 风险，保证 `ref_count` 与 `pending_io` 绝不下溢。
- **RCU / EBR 形式化生命周期验证与 512 读者高并发扩展**：新增 512 并发读者（256 静态槽位 + 256 动态溢出槽位）与 10,000 短生命周期线程形式化验证测试套件，证明动态槽位零泄漏、TID 安全复用与读者路径 100% 无锁 wait-free 执行；在 `csilk_server_set_router_full()` 中引入 `config_mutex` 对写端进行严格串行化，确保 Epoch 全局推进与回收链表挂载严格单调有序。
- **HTTP/2 流多路复用形式化生命周期验证**：形式化证明基于 16 个内嵌 Bucket 哈希表及空闲池链表（`csilk_h2_stream_map_t`）的流生命周期，确保在 `RST_STREAM`、`GOAWAY`、连接异常断开、并发异步 Handler 触发时，流回调绝不访问已被回收的 Context / Client。
- **PMU 硬件性能计数器引导的微优化**：对核心热点路径（`client_ref`/`client_unref`、`pending_io_inc`/`pending_io_dec`、RCU 嵌套深度追踪、`g_dispatch_tls_registered` 分支缓存及 HeaderMap 快速掩码短路）进行微架构优化，在不破坏正确性与形式化证明的前提下降低 CPU 周期数与缓存未命中。
- **Wait-Free MPSC 队列哨兵空安全加固**：在 `csilk_lfq_dequeue()` 中加入空指针防护，并在 Worker Pool 初始化时自动初始化跨线程分发队列，确保在任意线程拓扑下的极端鲁棒性。
- **服务器关机与销毁生命周期有序排空**：严格规范 `server_stop` -> active clients 关闭 -> 定时器排空 -> dispatch 队列排空 -> worker join -> hot reload 停止 -> EBR 宽限期 -> 释放 router -> 关闭 event loop -> 释放 pool 顺序；将 MQ 销毁推迟至 Worker 线程全部 Join 完成之后，防止 Worker 退出期间异步触发 MQ 悬空访问。

### 修复
- **Dispatch Async 资源释放与事件循环排空**：在 `csilk_server_free()` 中安全关闭 `wp->dispatch_async` 句柄并排空事件循环，防止 libuv 默认事件循环中残留悬空句柄。

## [0.5.0] - 2026-08-22

### 新增
- **Hot-Reload 控制面互斥并发保护与安全临时文件**：在 `hot_reload_ctx_t` 中引入 `csilk_mutex_t reload_mutex`，保证文件系统变更防抖与跨线程手动 `csilk_dev_hot_reload_trigger()` 互斥执行，消除状态竞争；强制采用 `mkstemp(0600)` 原子创建临时二进制文件，并在 OOM/错误时执行完整回滚（`dlclose`, `unlink`, `csilk_router_free`）。
- **io_uring 队列自适应阶梯缩放与资源降级**：将硬编码 4096 深度改为自适应阶梯初始化（`1024 -> 512 -> 256 -> 128 -> 64`），按需等比分配操作池，彻底消除在容器/VM 受限锁页内存（`RLIMIT_MEMLOCK`）下的 `-ENOMEM` 初始化失败。
- **统一 6 级内存所有权模型**：在 `<csilk/core/types.h>` 中规范定义 6 级内存所有权体系（`csilk_ownership_t`：`BORROWED`、`ARENA`、`OWNED`/`HEAP`、`TRANSFER`、`POOL`、`TLS_CACHE`）及字符串化函数 `csilk_ownership_str()`。在 `_csilk_ctx_cleanup()` 中统一容量感知型的缓冲池归还与堆内存清理，并强化响应体内存置换守卫（`_csilk_free_response_body_if_needed()`）。
- **3 层 ABI 架构与严格不透明句柄封装**：在 `include/` 下所有 52 个公共头文件中落地 `Public API → Opaque Handle → Internal Implementation` 架构：
  - 将 `<csilk/core/router.h>` 中的 `csilk_router_t` 彻底转为不透明句柄，将 `struct csilk_router_s` 与 Trie 节点结构下沉至 `src/core/primitives/router_internal.h`。
  - 解耦公共头文件中的 OpenSSL 依赖：`<csilk/core/hash.h>` 采用 64-bit 内存对齐的 128 字节不透明缓存定义 `csilk_sha1_ctx` 与 `csilk_sha256_ctx`，并在编译期通过 `_Static_assert` 校验尺寸。
  - 剥离底层 I/O 句柄：从 `<csilk/core/context.h>` 中移除 `csilk/core/sys_io.h` 包含，将 `csilk_get_work_req` 移至 `src/core/ctx/ctx_internal.h`。
- **异步 Context 生命周期安全与代数追踪**：新增活跃异步引用计数（`_csilk_ctx_async_ref_incr` / `_csilk_ctx_async_ref_decr`）、严格递增的请求代数序列号（`request_seq`）与 UUID v4 唯一请求标识（`request_id`），杜绝异步 Worker、数据库或 MQ 回调在 Keep-Alive 连接复用后操作过期 Context 或引发 UAF。
- **连接生命周期状态机**：实现显式 9 状态连接生命周期状态机（`csilk_conn_state_t`：`INIT`、`ACCEPTED`、`TLS`、`READING`、`PROCESSING`、`WRITING`、`STREAMING`、`CLOSING`、`CLOSED`）与严格的状态转移不变式校验（`csilk_conn_set_state`、`csilk_conn_get_state`、`csilk_conn_state_str`），彻底消除 UAF、Double Close、Double Free 以及异步流式/Keep-Alive 状态竞态。
- **I/O 与并发抽象层**：在 `<csilk/core/sys_io.h>` 与 `<csilk/core/sync.h>` 中规范统一跨后端 I/O 原语 `csilk_io_*`、跨平台线程抽象 `csilk_thread_*`（`csilk_thread_create`, `csilk_thread_join`, `csilk_thread_self`, `csilk_thread_setaffinity`）以及屏障 `csilk_barrier_*`（`csilk_barrier_init`, `csilk_barrier_wait`, `csilk_barrier_destroy`）。

- **流式背压与高低水位流量控制**：为 HTTP/1.1 分块流（`csilk_response_write`）、SSE（`csilk_sse_send`）及 WebSocket（`csilk_ws_send`）增加连接级出站队列背压机制。支持配置高水位线（`write_high_water_mark`，默认 64KB）、低水位线（`write_low_water_mark`，默认 16KB）、最大排队限制（`max_write_buffer_size`，默认 16MB）及异步排空回调注册（`csilk_on_drain` / `csilk_set_write_watermarks`）。
- **Context 存储析构器支持（RAII）**：新增 `csilk_set_ex()` 支持传入自定义析构函数（`csilk_destructor_t`），在请求结束释放 Arena 时自动清理堆内存对象；JWT 中间件自动为 `jwt_payload` 绑定 `csilk_json_free` 析构，防止内存泄漏。
- **强类型零拷贝视图**：新增 `csilk_view_t`（`const char* data; size_t len;`）及借用语义 Getter（`csilk_get_query_view`、`csilk_get_param_view`、`csilk_get_header_view`、`csilk_get_body_view`），明确区分指向解析缓冲区的零拷贝借用与 Arena 分配的以 NUL 结尾的所有权字符串。
- **JWT 验证策略与选项配置**：新增 `csilk_jwt_flags_t`（`CSILK_JWT_REQUIRE_EXP`、`CSILK_JWT_REQUIRE_NBF`、`CSILK_JWT_REQUIRE_IAT`）、`csilk_jwt_options_t`（算法、策略标志、时钟容差）、`csilk_jwt_verify_options()` 与 `csilk_jwt_middleware_options()`，提供明确且严格的声明校验策略。
- **Arena Calloc 与多级 TLS 缓存**：新增 `csilk_arena_calloc()` 支持零初始化内存分配；引入 4KB / 16KB / 64KB 三级线程局部 Chunk 空闲链表与 `max_total_bytes` 约束，并在 Worker 线程退出时自动清理（`csilk_arena_flush_free_list`）。

- **加密驱动扩展性**：`csilk_crypto_driver_t` 新增 `sha1`（20 字节摘要）与 `bcrypt_hash`（密码哈希）回调，配套内部分发包装 `_csilk_sha1()`、`_csilk_bcrypt_hash()`——驱动可替换内置软件实现。
- **`csilk_cond_broadcast()`**：在 `<csilk/core/sync.h>` 中新增条件变量广播函数，支持一次性唤醒所有等待者，弥补 libuv 无 broadcast 原语的缺口。
- **Crypto 模块测试**：在 `tests/crypto/test_crypto.c` 中添加覆盖 SHA-256、HMAC-SHA256、Base64/Base64URL 往返、`csilk_crypto_fill_random`、`csilk_crypto_generate_nonce` 及 `csilk_url_decode` 边界情况的属性测试。

### 变更
- **统一委托 OpenSSL 密码学原语**：将手写的 SHA-256（`csilk_sha256_*`）、HMAC-SHA256（`csilk_hmac_sha256`）和 SHA-1（`csilk_sha1_*`）完全替换为成熟的系统级 OpenSSL 原语实现；重新实现 `bcrypt` 算法底层，集成 OpenSSL `RAND_bytes()`/`RAND_priv_bytes()` 强随机源、`CRYPTO_memcmp()` 恒定时间比对、`OPENSSL_cleanse()` 内存安全擦除与纯栈上重入线程安全状态机。
- **Release 模式默认可移植二进制**：将 Release 构建的默认选项调整为生成兼容性更高的可移植二进制（默认不加 `-march=native`），防止在较旧 CPU、Docker 容器或 CI 分发制品中触发 `SIGILL` 非法指令崩溃。本机指令集深度优化调整为显式开启 `-DCSILK_ENABLE_NATIVE_ARCH=ON`（并在 Benchmark 压测脚本和 CI 性能工作流中自动启用）。

- **Core 核心层纯净抽象解耦**：彻底消除 `src/core/server/`（`connection.c`, `server_lifecycle.c`, `server_shutdown.c`, `server_worker.c`）中对 `uv_*` 的直接依赖，统一调用 `csilk_io_*` 与 `csilk_thread_*`/`csilk_barrier_*`。


- **io_uring 架构精简与合并**：消除原冗余的 `uring_server.c`、`uring_connection.c`、`uring_event_loop.c` 副本，将驱动精炼统一至 `src/core/uring/uring_io.c`，实现全后端单轨执行。
- **Router 前缀树架构文档对齐与回滚**：更新 Router 文档以准确描述 Segment-based 前缀树架构；修复通配符路径匹配在 Method 不匹配或 Handler 缺失时的参数回滚机制。
- **Handler 链越界安全检查**：在 `csilk_next()` 中增加显式 `handler_count` 边界校验，防止异常 Handler 数组导致越界访问。
- **线程隔离与分发规范**：明确规范 Worker 线程私有 `active_clients` 隔离语义，跨线程操作统一使用 `csilk_dispatch()` 进行异步分发。
- **Barrier 生命周期**：`src/core/server/server_lifecycle.c` 中的 `uv_barrier_t` 改为堆分配（`calloc`），防止多线程 worker 在栈上的 barrier 被销毁后仍持有该地址导致的 UAF。现在检查 `uv_barrier_init` 返回值。
- **线程抽象统一**：`src/core/uring/uring_thread_pool.c` 中将原始 `pthread_mutex_t`/`pthread_cond_t` 替换为 `<csilk/core/sync.h>` 中的 `csilk_mutex_t`/`csilk_cond_t`，保持跨后端一致性。
- **头文件卫生**：从 `include/csilk/core/internal.h` 移除隐式包含 `messaging/mq_internal.h`，需使用 `_csilk_mq_new`/`_csilk_mq_free` 的文件现在显式 include。
- **代码清理**：将 1200+ 处 `nullptr` 统一替换为 `NULL` 以符合 C23 风格；修复 connection.c 的 `-Wcomment`、qdrant.c 和 workflow_dsl.c 的 `-Wformat`、session.c 的 `strdup` null 检查。

### 修复
- **csilk_server_free 释放自有事件循环**：在 `csilk_server_free` 中为 `server->loop_owned` 增加 `csilk_io_loop_close` 与内存回收，消除 io_uring 模式下创建多实例时的文件描述符泄漏。
- **多 Worker Sendfile 与启动 Hook 同步**：在多 Worker 文件传输测试中采用 `CSILK_HOOK_SERVER_START` Hook 精确同步主/从 Worker 状态，并加入指数退避重试，消除并发死锁。
- **io_uring 后端定向取消与监听套接字安全**：修复 `csilk_io_timer_stop()` 使用基于指针与 generation 标记的 `io_uring_prep_cancel64()` 进行精准定向取消，防止定时器取消误伤监听套接字 SQE。
- **io_uring 异步与信号轮询唤醒可靠性**：在 `URING_OP_POLL_ASYNC` 与 `URING_OP_POLL_SIGNAL` 事件分发前增加 `read() > 0` 校验，防止伪就绪事件触发错误回调。
- **双后端构建字段隔离与跨后端兼容**：在 `src/core/server/connection.c` 中通过 `#ifdef CSILK_USE_URING` 严格隔离 `generation` 与 `fd` 成员访问，并引入统一的 `reject_connection()` 辅助函数，确保 libuv 模式与 io_uring 模式下 100% 编译通过且全量测试通过。
- **Router 路由匹配日志空指针防护**：在 `csilk_router_match_ctx()` 日志输出中增加 `mh` 非空防御检查，彻底解决 `clang-tidy` 静态分析下的 `clang-analyzer-core.NullDereference` 报错。
- **多 Worker 启动屏障死锁**：消除多 Worker 初始化阶段在内存分配（`worker_data_t`）或线程创建（`csilk_thread_create`）失败时的永久死锁问题，对未启动 Worker 进行屏障补偿并执行安全回滚与资源清理。
- **TCP 读取缓冲区动态扩容**：将 `read_buffers` 改为动态扩容（初始 16，按需倍增），解决单个请求超过 16 次 TCP Read 时后续数据静默丢失的问题。
- **原子最大连接数预留**：将 `max_connections` 检查改为原子 CAS 预留（`_csilk_server_try_acquire_connection`）与回滚，彻底消除高并发下的 TOCTOU 竞态。
- **JWT 内存泄漏**：在 JWT 中间件中使用 `csilk_set_ex()` 绑定析构器，解决 cJSON payload 在请求结束时未释放的问题。
- **uv_barrier_t UAF**：修复多 worker 服务器启动时的 use-after-free——主线程在 `uv_barrier_destroy` 后栈变量析构，而 worker 线程仍持有其地址。现改为堆分配并在所有 worker join 后释放。
- **internal.h MQ 泄漏**：从 `include/csilk/core/internal.h` 移除 `#include "messaging/mq_internal.h"`，避免所有 include 该头文件的代码都暴露 MQ 内部类型（如 `csilk_mq_t`）。

## [0.4.0] - 2026-08-13

### 变更
- **目录结构重组**：将 `base64.c`、`sha1.c`、`url.c`、`uuid.c`、`crypto.c`（原名 `utils.c`）从 `src/core/server/` 移至新模块 `src/crypto/`；将 `bcrypt.c` 和 `blowfish_sboxes.h` 并入 `src/crypto/`（合并 `src/security/`）；将 `admin.c` 从 `src/core/config/` 移至 `src/app/`；将测试从 `tests/data/` 重组至 `tests/security/` 和 `tests/drivers/db/`；删除冗余的 `include/csilk/core/admin.h` re-export 包装；移除 `CSILK_DATA_SOURCES` CMake 变量（内联至 `CSILK_DRIVER_SOURCES`）。

### 新增
- **原生内嵌式 SIMD 向量检索索引引擎**：32 字节内存对齐 AVX2 SIMD 距离算子（Cosine / L2 / 点积）与多层 HNSW 跳表图索引 (`csilk_hnsw_index_t`)，实现 $O(\log N)$ ANN 近似最近邻向量检索与全零依赖内嵌驱动 (`csilk_vector_db_new_embedded`)。
- **eBPF XDP 动态规则 WAF 与 OTLP 全链路追踪 Web 仪表盘**：BPF-Map 无缝热加载内核防火墙规则 (`csilk_xdp_waf_add_ip_rule`)、W3C 链路追踪 2048-Span 无锁环形缓冲区 (`csilk_otlp_tracer_start_span`)，以及单页嵌入式 Web APM Dashboard (`share/csilk/apm_ui.html`, `/admin/apm`)。

### 安全
- **敏感缓冲区清零**：在 csrf、jwt、session 和 websocket 模块中使用后清零敏感缓冲区，防止数据泄漏。
- **JWT 整数溢出保护**：在 JWT 解析中为 base64 长度计算添加溢出保护。

### 修复
- **bcrypt 空密码验证失败**：修复 bcrypt 实现中的三个导致 `csilk_bcrypt_verify` 对空密码始终返回 -1 的 bug：(1) `CSILK_BCRYPT_CIPHER_OUT` 为 23 而非 24——24 字节应编码为 32 个 base64 字符，但 verify 只读取 31 个，丢失了最后一个字节；(2) Eksblowfish P-array 密钥调度（Step 2）前未将 `datal`/`datar` 初始化为零，导致空密码时 salt 与栈垃圾值异或；(3) `pwd_buf` 在 `memcpy` 前未 `memset` 清零，`len == 0` 时留下未初始化内存。将 `CSILK_BCRYPT_HASH_LEN` 从 61 更新为 62 以匹配修正后的哈希格式（`$2a$XX$` + 22 salt + 32 checksum + NUL）。
- **clang-tidy 误报**：在 `blowfish_encipher` 的 `XL ^= p[i]` 处抑制 `clang-analyzer-core.uninitialized.Assign`——分析器无法追踪数组指针参数；代码正确。
- **编译警告**：修复 connection.c 中块注释内无效的 `/*`（`-Wcomment`）；修复 qdrant.c 中对 `int64_t` 使用 `%lld` 格式符（`-Wformat`）；修复 workflow_dsl.c 中 snprintf 多余的 NULL 参数（`-Wformat`）；为 crypto.h 和 crypto_dispatch.h 中的 bcrypt 签名应用 clang-format。
- **Python wheel 打包**：移除 `setup.py` 中多余的 `csilk/` 子目录路径；在 CMakeLists.txt 中添加 `if(NOT DEFINED)` 守卫，确保 setuptools 传入的 `-D` 值不被覆盖。
- **macOS rpath**：为共享库设置 `@loader_path` rpath，兼容 delocate-wheel。
- **路由宏安全性**：将路由宏包装在 `do { } while(0)` 中，以便在控制流语句中安全使用。
- **CI 兼容性**：升级 upload/download-artifact 到 v6 以支持 Node 24，在未收集到样本时跳过 FlameGraph 上传，修复 benchmark-results 上传路径。
- **macOS 兼容性**：为 macOS 构建添加可移植的 `explicit_bzero` 兼容层。

### 变更
- **头文件保护现代化**：将所有 38 个公共头文件中的 `#ifndef`/`#define` 头文件保护替换为 `#pragma once`。
- **API 文档**：为 middleware、server 和 group 头文件中未文档化的公共 API 函数添加 Doxygen 文档。
- **tag-release.sh**：扩展以覆盖所有版本位置——`src/` `.c` `@version`、`python/csilk/_version.py`、`cmake/ports/csilk/vcpkg.json`、`vX.Y.Z+` 文档头部、`| Version: X.Y.Z` 元数据、ASCII 图版本以及 `version: X.Y.Z` 代码块引用。

## [0.3.0] - 2026-06-27

### 新增
- **io_uring 后端（仅 Linux，可选）**：使用 `CSILK_USE_URING=ON` 构建时支持完整的事件循环、accept、read、write 和 timer 实现。SQPOLL（Square-Submission-Polling）支持自动回退。每工作线程线程池带无锁分发队列。全部 122 个测试通过。
- **文档**：更新所有文档（架构、构建指南、测试指南、部署、性能调优、故障排查、设计），全面覆盖 io_uring 后端。
- **零拷贝 HTTP 解析** — 集成 C23 风格的字符串视图（`csilk_str_view_t`），用于 HTTP 头、URL 和请求体，直接引用网络接收缓冲区，消除堆 malloc/free 开销。
- **深层结构体释放** — 添加 `csilk_struct_free_reflect`，在反射引擎内递归释放嵌套结构体指针。
- **静态循环引用检测** — 添加编译/启动时 DFS 图循环检测算法，验证已注册的反射类型，防止递归栈溢出。
- **CI 中的 Fuzz 测试**：重新启用 fuzz 测试任务（clang-19 预计在 2026 年 6 月前在 Ubuntu 24.04 上可用）。
- **扩展测试覆盖**：WAF（4→9）、Session（5→8）、Recovery（1→4）、CSRF（3→7）、Workflow Lifecycle（1→3）。
- **零拷贝分块写入**：`_csilk_send_data_owned()` 消除分块传输编码路径中的双重分配/拷贝。
- **ABI 不透明类型转换**：将内部结构体定义（`csilk_ctx_s`、`csilk_server_s`）从 `include/csilk/core/` 移至 `src/core/`。所有非框架代码现通过公共访问器 API 访问上下文状态。
- **延迟清理 API**（`csilk_ctx_defer` / `csilk_ctx_defer_free`）：跨 `setjmp`/`longjmp` 边界的 panic 安全资源管理。防止堆分配、文件描述符和互斥锁在 panic 恢复期间泄漏。
- **SIMD 加速路由**：x86_64 上的 AVX2 路径匹配和 aarch64 上的 ARM NEON。CMake 自动检测配合 `-mavx2` 标志。
- **无锁 per-worker 连接池**：用 per-worker 无锁池替换基于互斥锁的连接池，实现多核扩展。
- **macOS 14 ARM64 CI 支持**：在 CI 矩阵中重新启用 macOS，包含 `fdatasync`→`fsync` 和 `SOCK_NONBLOCK` 回退。
- **实时 CPU 火焰图**：管理后台中的 Backtrace 采样和火焰图渲染。
- **TypeScript/Python SDK 生成**：从 OpenAPI 规范自动生成 API 客户端。
- **动态 AI 工具发现**：面向 agentic 工作流的 MCP 风格工具发现 API。
- **常量时间 JWT 签名比较**：用常量时间比较替换 `strcmp`。
- **Python 脚手架工具**：将 `csilkskel` 从 C 重写为交互式 Python 工具。
- **热重载支持**：`csilk_server_set_router` 用于运行时路由替换。
- **HTTP/2 第一阶段 — Session 框架**：TLS ALPN 协商（`h2` vs `http/1.1`）、nghttp2 session 初始化、`csilk_h2.h` 公共 API，包含 `csilk_h2_init_session`、`csilk_h2_process_data`、`csilk_h2_get_or_create_stream`、`csilk_h2_free_streams` 和用于帧序列化的 `send_callback`。
- **HTTP/2 第二阶段 — 请求分发与响应**：提取 `_csilk_dispatch_request` 用于 HTTP/1.1 和 HTTP/2 的统一路由。实现 nghttp2 回调（用于伪头部 + 常规头部解析的 `on_header_callback`、用于 END_STREAM 分发的 `on_frame_recv_callback`、用于请求体积累的 `on_data_chunk_recv_callback`、用于上下文清理的 `on_stream_close_callback`）。添加 `csilk_h2_send_response`，带用于流式响应体的 `body_read_callback` 数据提供器。
- **`test_h2` 测试套件**：在 `cmake/tests.cmake` 中注册。
- **C23 语言标准**：从 C11 升级到 C23（`CMAKE_C_STANDARD 23`）。将 `#define` 常量转换为 `static constexpr`，实现类型安全的编译时值。移除 6 行 `#include <stdbool.h>`（现为 C23 关键字）。
- **Form URL-encoded 解析器**：添加 `csilk_parse_form_urlencoded()` 和 `csilk_get_form_field()`，用于 `application/x-www-form-urlencoded` 请求体解析（P5-1）。
- **Session 支持**：基于 Cookie 的内存 Session 管理，提供 `csilk_session_init/start/set/get/destroy` API（P5-2）。
- **HTTP Range 请求**：静态文件中间件现在支持 `Range` 头，返回 206 Partial Content 响应（P5-3）。
- **请求验证中间件**：`csilk_validate()` 支持 REQUIRED/INT/STRING/EMAIL 标志和 min/max 范围验证（P5-4）。
- **连接对象池**：通过空闲列表复用 `csilk_client_t` 对象，减少分配开销（P3-5）。
- **URL 解码**：实现 `csilk_url_decode()` 用于百分比解码查询参数。
- **SHA1/Base64 已知答案测试**：14 个测试用例覆盖 RFC 3174 和 RFC 4648 向量。
- **WebSocket 集成测试**：验证 101 Switching Protocols + `Sec-WebSocket-Accept` 头。
- **流式响应集成测试**：验证使用 `csilk_response_write/end` 的分块编码。
- **重定向测试**：使用 `csilk_redirect_simple` 进行了增强，覆盖 301/302/307 状态码和空安全边界情况。

### 变更
- **原子内置函数标准化** — 将所有遗留的编译器依赖 GCC `__sync_*` 原子操作替换为标准的 C11 `<stdatomic.h>` API。
- **多 worker 循环安全性** — 移除硬编码的 `uv_default_loop()` 引用，动态解析活跃工作线程的事件循环，防止多 worker 数据竞争。
- **HTTPS 读取路径优化** — SSL 读缓冲区现在从连接 arena 而非栈上分配，确保解密数据对零拷贝字符串视图安全。
- **Arena 安全性**：在 `csilk_arena_alloc` 中添加溢出防护和零大小哨兵处理。
- **中间件数量**：将 WAF（Web 应用防火墙）添加到 15 个内置中间件中。
- **管理后台存储限制测试**：修复 `test_admin` 存储溢出问题，以存储非空值。
- **`_csilk_trigger_hooks`**：改为非静态，并在 `server_internal.h` 中声明，以便 H2 模块可以触发生命周期钩子。
- **`pool_put`**：现在调用 `csilk_h2_free_streams` 清理任何 H2 流上下文，然后再将客户端返回到空闲池。
- **版本号提升**：所有 18 个版本引用从 0.2.5 → 0.3.0。
- **常量迁移**：`CSILK_DEFAULT_*`（5 个）、`CSILK_MAX_PARAMS`、`CSILK_MAX_STORAGE`、`CSILK_MAX_CHILDREN`、`MAX_REG_STRUCTS`、`MAX_IP_ENTRIES`、`WINDOW_SIZE` 转换为 `static constexpr` 并移至适当的头文件。
- **连接池**：池大小为 32 个客户端；在 `csilk_server_free` 中排空池。
- **流式响应**：`csilk_response_write/end` 现在设置 `is_async` 标志以防止双重写入；分块头尊重客户端的 `Connection: close` 头。
- **静态中间件**：在所有静态响应中添加 `Accept-Ranges: bytes` 头。
- **流式清理**：终端分块写入回调关闭连接，而不是将清理留给定时器（修复 use-after-free）。
- **头文件位置**：`context_internal.h` 从 `src/core/` 移至 `include/`；从 CMakeLists.txt 中移除 `src/core` 包含路径。
- **Doxygen 文档**：在所有 37 个源/头文件中完成完整的 Doxygen 注释，包含 `@brief`、`@param`、`@return`、`@note` 注解。

### 修复
- **io_uring SQE 饥饿**：当 io_uring 提交队列环满时，`csilk_client_write` 可能静默丢弃响应。添加了带退避的重试循环。
- **on_write_done 中的陈旧 keep_alive**：llhttp 9.3.1 在 `on_message_complete` 返回后清除 `F_CONNECTION_CLOSE`，导致写入完成回调中 `llhttp_should_keep_alive()` 返回错误值。在 `_csilk_send_response` 中计算时将决策缓存在 `client->keep_alive` 中。
- **零拷贝表单体解析**：修复 `csilk_parse_form_urlencoded`，当零拷贝 HTTP 体引用 llhttp 的 TCP 缓冲区（在体边界处不以 null 结尾）时，使用显式体长度（`csilk_arena_strndup` 而非 `csilk_arena_strdup`）。
- **ASan 泄漏**：解决新测试和 Doxyfile 生成中的内存泄漏。
- **macOS 兼容性**：`fdatasync` → `fsync`，`SOCK_NONBLOCK` 处理。
- **CI ASan 抑制**：添加对 macOS 误报的抑制。
- **Arena TLS 空闲列表泄漏**：添加 `csilk_arena_flush_free_list()`，在 server 释放时调用，防止服务器在非主线程上运行时出现 ASAN 检测到的泄漏。
- **MQ realloc 溢出**：在 monitor 数组、全局中间件数组和 per-topic 处理器数组的扩容路径中添加整数溢出防护和 NULL 检查。
- **`csilk_db_query_param_json` 中的 SQL 注入**：添加了标准 SQL 单引号双写转义。
- **HTTP 解析器内存泄漏**：`on_url` URL 超限、`on_header_value` 大小超限/缓冲区增长失败现在会释放 `current_url`、`current_header_field` 和 `current_header_value`。
- **app.c 错误路径 server 泄漏**：当 `csilk_router_new()` 失败时 `csilk_server_new(NULL)` 仍成功；在失败路径中添加 `csilk_server_free()`。
- **hot_reload.c 资源泄漏**：当 `dlsym`/`GetProcAddress` 或 init 函数失败时，`dlclose`/`FreeLibrary` 未被调用。
- **WAF 空上下文段错误**：`csilk_waf_middleware(nullptr)` 在未阻塞路径上调用 `csilk_next(nullptr)` 时崩溃。
- **4 个 const 限定符警告**：`bounded_buf.c` 返回类型和 `static.c` C23 `strchr` 重载。
- **GCC 内置原子操作**：`perm.c` `__sync_val_compare_and_swap` → C11 `atomic_compare_exchange_strong`。
- 修复 `csilk_parse_form_urlencoded` 的 Content-Type 检查逻辑（严格的 `application/x-www-form-urlencoded` 检查）。
- 修复静态中间件中的内存泄漏：完整文件缓冲区的 `body_is_managed = 1` 确保清理。
- 修复流式响应生命周期中的 `csilk_ctx_cleanup` + 定时器交互。
- 修复 server.c（pool_get/pool_put 参数类型）和 session.c（typedef）中的 3 处 `csilK_` 拼写错误。

## [0.2.5] - 2026-05-29

### 修复
- **多 worker 模式下的客户端池数据竞争**：`pool_get`/`pool_put` 访问 `client_pool` 和 `client_pool_count` 时没有同步。在多 worker 模式下，`on_new_connection` 可在任何事件循环线程上运行，导致两个线程获取同一个客户端对象。这触发了 libuv 断言崩溃：`uv_accept: Assertion 'server->loop == client->loop' failed`。添加 `pool_mutex` 保护所有池操作。

## [0.2.4] - 2026-05-28

### 新增
- **Redis 数据库驱动**：新的 `src/drivers/redis.c` 驱动，使用 hiredis。支持连接池，带密码认证和数据库索引选择。将 Redis 回复类型映射为表格结果：GET→1 行，HGETALL→字段/值对，KEYS/LRANGE→N 行扁平数组。支持 MULTI/EXEC/DISCARD 事务。

## [0.2.3] - 2026-05-28

### 新增
- **统一管理后台**：`/admin` 路径的 Web 实时监控仪表盘，包含 HTTP 指标、工作流执行图、MQ 队列状态、数据库池遥测、AI 模型调用追踪和进程指标。提供带 WebSocket 实时事件的 `admin_ui.html` SPA。
- **MongoDB 数据库驱动**：新的 `src/drivers/mongodb.c` 驱动，使用 libmongoc。支持连接池和统一的数据库查询接口。
- **MQ 消息状态监控**：实时 MQ 事件、深度追踪和 JSON 统计端点，用于管理后台集成。
- **全局 AI 遥测**：`src/ai/ai.c` 现追踪模型调用、token 计数和延迟，供管理后台使用。
- **全局 DB 遥测**：`src/data/db.c` 追踪所有数据库驱动的池大小、活跃连接和查询延迟。

### 修复
- **test_workflow_monitor SEGFAULT**：修复由 `calloc(1, 1024)` 引起的堆缓冲区溢出 — `csilk_ctx_t` 为 2944 字节，分配的缓冲区过小。在 ASan 下，每次 CI 运行都会触发 SEGFAULT。
- **scaffold `csilk_perm_auto_middleware_passthrough`**：替换为现有的 `csilk_perm_auto_middleware` — 前者从未存在过，导致核心 API + perm 模式编译失败。
- **MQ 恢复回归**：修复连接断开后的消息队列恢复。
- **Admin 结构体隐私**：解决 admin 模块中 `csilk_ctx_t` 的不完整类型问题。
- **Mermaid 语法**：修复工作流 Mermaid 可视化的版本 10+ 引号问题。
- **test_timeout 不稳定**：通过添加服务器就绪同步修复端口冲突。

### 变更
- **头文件搬迁**：`workflow_wal.h` 从 `src/app/` 移至 `include/csilk/app/`，使所有头文件位于 `include/` 下。
- **Admin 脚手架**：`csilkskel` 现在默认包含管理后台设置。
- **版本号提升**：0.2.1 → 0.2.3

## [0.2.2] - 2026-05-27

### 新增
- **对称/非对称密码驱动**：新的 `csilk_cipher_driver_t` 接口，支持 AES-256-GCM 加密/解密、RSA-2048 密钥生成、RSA-OAEP 加密/解密和 RSA-PSS 签名/验证。包含默认的 OpenSSL EVP 实现（`src/crypto/cipher.c`）。通过 `csilk_server_set_cipher_driver()` 可插拔 — 传入 NULL 恢复默认值。
- **密码测试**：8 个测试用例覆盖对称往返、错误标签拒绝、错误密钥拒绝、非对称往返、签名/验证、自定义驱动插件、自定义密钥生成和 NULL 上下文回退。

### 变更
- **`csilk_ctx_t`**：添加 `cipher_driver` 字段，用于每次请求的密码访问。
- **项目结构**：添加 `src/crypto/` 和 `include/csilk/drivers/cipher.h`。

## [0.2.1] - 2026-05-25

### 新增
- **Form URL-encoded 解析器**：添加 `csilk_parse_form_urlencoded()` 和 `csilk_get_form_field()`，用于 `application/x-www-form-urlencoded` 请求体解析（P5-1）。
- **Session 支持**：基于 Cookie 的内存 Session 管理，提供 `csilk_session_init/start/set/get/destroy` API（P5-2）。
- **HTTP Range 请求**：静态文件中间件现支持 `Range` 头，返回 206 Partial Content 响应（P5-3）。
- **请求验证中间件**：`csilk_validate()` 支持 REQUIRED/INT/STRING/EMAIL 标志和 min/max 范围验证（P5-4）。
- **连接对象池**：通过空闲列表复用 `csilk_client_t` 对象，减少分配开销（P3-5）。
- **URL 解码**：实现 `csilk_url_decode()` 用于百分比解码查询参数。
- **SHA1/Base64 已知答案测试**：14 个测试用例覆盖 RFC 3174 和 RFC 4648 向量。
- **WebSocket 集成测试**：验证 101 Switching Protocols + `Sec-WebSocket-Accept` 头。
- **流式响应集成测试**：验证使用 `csilk_response_write/end` 的分块编码。
- **重定向测试**：使用 `csilk_redirect_simple` 进行了增强，覆盖 301/302/307 状态码和空安全边界情况。

### 变更
- **连接池**：池大小为 32 个客户端；在 `csilk_server_free` 中排空池。
- **流式响应**：`csilk_response_write/end` 现设置 `is_async` 标志以防止双重写入；分块头尊重客户端的 `Connection: close` 头。
- **静态中间件**：在所有静态响应中添加 `Accept-Ranges: bytes` 头。
- **流式清理**：终端分块写入回调关闭连接，而不是将清理留给定时器（修复 use-after-free）。
- **头文件位置**：`context_internal.h` 从 `src/core/` 移至 `include/`；从 CMakeLists.txt 中移除 `src/core` 包含路径。
- **Doxygen 文档**：在所有 37 个源/头文件中完成完整的 Doxygen 注释，包含 `@brief`、`@param`、`@return`、`@note` 注解。

### 修复
- 修复 `csilk_parse_form_urlencoded` 的 Content-Type 检查逻辑（严格的 `application/x-www-form-urlencoded` 检查）。
- 修复静态中间件中的内存泄漏：完整文件缓冲区设置 `body_is_managed = 1` 确保清理。
- 修复流式响应生命周期中的 `csilk_ctx_cleanup` + 定时器交互。
- 修复 server.c（pool_get/pool_put 参数类型）和 session.c（typedef）中的 3 处 `csilK_` 拼写错误。

## [0.2.0] - 2026-05-23

## [0.1.0] - 2026-05-15

### 新增
- 初始版本，包含核心路由、中间件和服务器实现。
- 支持 JSON（cJSON）、WebSocket 和 YAML 配置。
- 内置中间件：Logger、Recovery、Auth、CORS、CSRF、Rate Limiting、Static Files。
- 完整的 Doxygen 文档。
