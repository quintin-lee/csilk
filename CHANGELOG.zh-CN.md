# 更新日志

本文件中记录了本项目所有值得注意的变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)，
本项目遵循 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [Unreleased]

### 安全
- **敏感缓冲区清零**：在 csrf、jwt、session 和 websocket 模块中使用后清零敏感缓冲区，防止数据泄漏。
- **JWT 整数溢出保护**：在 JWT 解析中为 base64 长度计算添加溢出保护。

### 修复
- **clang-tidy 警告**：解决源文件和测试文件中的静态分析警告，包括数组越界、内存泄漏和空指针解引用。
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
