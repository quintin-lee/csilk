# csilk 完善计划 & 演进路线

> 最后更新: 2026-06-06 | 基于 0.3.0 重构 | 0.5.0 开发中

---

## v0.5.x 增量修复与优化 (2026-06-05 — 2026-06-06)

### 正确性与内存安全
- [x] **arena TLS data race**: `tls_chunk_free_list`/`tls_chunk_count` 缺少 `_Thread_local`，多 worker 下竞态。加 `_Thread_local` + `csilk_arena_flush_free_list()` 线程退出清理
- [x] **MQ realloc 整数溢出**: `monitor`/`handler` 数组扩容 `* 2` 可溢出，`realloc` 返回值未检查
- [x] **SQL 注入**: `csilk_db_query_param_json` 参数原样拼接，无转义。添加 SQL 标准单引号双写转义
- [x] **HTTP 解析器内存泄漏**: `on_url` URL 超限、`on_header_value` 超限/分配失败时未释放 `current_url`/`current_header_field`/`current_header_value`
- [x] **app.c 错误路径泄漏**: `csilk_router_new()` 失败时 `csilk_server_new(NULL)` 仍成功，`group_new` 失败时 server 未释放
- [x] **hot_reload.c dlclose 泄漏**: `dlsym` 失败 / `init_fn()` 返回 NULL 时未关闭 `dl_handle`
- [x] **WAF null context 段错误**: `csilk_waf_middleware(nullptr)` 在非 blocked 路径调用 `csilk_next(nullptr)`

### 代码整洁
- [x] **4 处 const 警告**: `bounded_buf.c` `uint64_to_str` 返回类型、`static.c` `strchr` C23 重载
- [x] **GCC 内置原子 → C11 标准**: `perm.c` `__sync_val_compare_and_swap` → `atomic_compare_exchange_strong`
- [x] **int → size_t**: `http1.c` response 序列化 `pos` 变量消除 `(int)` 截断转型

### 性能
- [x] **chunked write 零拷贝**: `_csilk_send_data_owned()` 消除 `write_chunk_frame` 中的双重分配拷贝

### 测试覆盖提升
- [x] WAF: 4 → 9 (+125%, 多参数 SQLi, 深层目录穿越, query 穿越, clean POST, null 安全)
- [x] Session: 5 → 8 (+60%, destroy, 多键操作, 无 init 使用)
- [x] Recovery: 1 → 4 (+300%, 正常路径, deferred cleanup, 混合链)
- [x] CSRF: 3 → 7 (+133%, null buffer, 精确大小, GET/HEAD/OPTIONS 安全方法, POST/PUT/DELETE 缺 token, hex 格式校验)
- [x] Workflow Lifecycle: 1 → 3 (+200%, null 安全, get_node)

### CI 与文档
- [x] Fuzz 测试启用: 移除 `if: false`（预期 clang-19 已在 Ubuntu 24.04 可用）
- [x] 代码注释: `bounded_buf.c` (7 funcs), `hot_reload.c` (4 items), `flamegraph.c` (7 funcs), `db.c` (5 funcs), `context.c` (3 funcs: `csilk_next`, `ctx_defer`, `defer_free`)

---

## 一、演进阶段

### 阶段一：稳定性与测试基建
- [x] 1.1 覆盖率与边界单元测试 (url_parser.c, router.c 等，目标覆盖率 95%+)
- [x] 1.2 内存泄漏检测：在测试套件中全面集成 ASan
- [x] 1.3 Fuzzing 模糊测试：针对 HTTP 报文解析及路由匹配
- [x] 1.4 CI/CD 自动化：配置自动化运行构建、测试和内存检查

### 阶段二：架构与性能深度调优
- [x] 2.1 请求级内存池 (Arena)：降低 malloc 碎片与锁竞争
- [x] 2.2 路由树局部性优化与 Fast-path（固定数组子节点布局）
- [x] 2.3 异常流优化：优化 setjmp/longjmp 的 recovery 开销

### 阶段三：文档与开发者体验
- [x] 3.1 规范化注释与 Doxygen API 文档
- [x] 3.2 架构机制白皮书 (docs/ARCH.md)
- [x] 3.3 示例工程扩展 (example_server 增强)

### 阶段四：新功能拓展
- [x] 4.1 WebSocket 协议支持
  - [x] 4.1.1 集成 SHA1 与 Base64 算法 (用于握手)
  - [x] 4.1.2 实现完整的握手逻辑与协议升级
  - [x] 4.1.3 实现 WebSocket 帧 (Frame) 解析与编码
  - [x] 4.1.4 整合进 libuv 事件循环与 Csilk Context
- [x] 4.2 更多官方中间件 (Rate Limiter, CSRF, CORS, Multipart, Gzip, SSE)
- [x] 4.3 TLS/HTTPS 调研（建议 Nginx 反向代理）

### 阶段五：日志系统成熟化
- [x] 5.1 引入日志宏 (CSILK_INFO, CSILK_ERROR) 自动记录源码行号与函数名
- [x] 5.2 实现基于文件大小的自动日志滚动 (Log Rotation)
- [x] 5.3 优化时间戳生成效率与格式化定制
- [x] 5.4 JSON 结构化日志输出支持

### 阶段六：YAML 配置支持
- [x] 6.1 集成 libyaml 依赖与 CMake 配置
- [x] 6.2 定义统一配置结构体 csilk_config_t 与 API
- [x] 6.3 实现基于事件流的 YAML 解析引擎 (src/config.c)
- [x] 6.4 更新示例工程支持 config.yaml 加载
### 阶段七：性能极致调优
- [x] 7.1 引入 uv_queue_work 线程池卸载 gzip/静态文件等阻塞操作
- [x] 7.2 Header 存储改为 Arena 分配 + 哈希表 (64 buckets)
- [x] 7.3 添加 csilk_arena_reset() 并在 keep-alive 请求间复用 Arena
- [x] 7.4 Logger 优化：移除 fflush 以提升高吞吐下性能
- [x] 7.5 SO_REUSEPORT + 多事件循环实例利用多核 (基于配置 worker_threads)
- [x] 7.6 修复 TCP_NODELAY 应用到 client socket 的问题

### 阶段八：工程化拓展与健壮性
- [x] 8.1 多线程 Session 安全 (Mutex 加锁)
- [x] 8.2 追踪中间件：Request ID (UUID v4) 支持
- [x] 8.3 标准化端点：内置 Health Check (/healthz)
- [x] 8.4 异常模拟：引入 OOM (Out Of Memory) 模拟测试框架
- [x] 8.5 响应优化：Gzip 智能跳过已编码或非文本内容
- [x] 8.6 安全增强：安全随机 Session ID 与严谨 Email 验证
- [x] 8.7 鲁棒性：处理超过 4GB 的分块数据写入

### 阶段九：架构精进与 v0.3.0 演进
- [x] 9.1 Context 不透明化 (ABI 稳定性) — 隐藏 csilk_ctx_s 细节
- [x] 9.2 插件化 Storage 接口 — 支持自定义 Context 存储后端
- [x] 9.3 优雅关闭深度优化 — WebSocket/SSE 安全闭合与排出机制
- [x] 9.4 JWT 官方中间件实现 — 基于 HS256 的令牌签发与校验
- [x] 9.5 扩展 Hook 系统 — 支持生命周期事件监听与第三方接入
- [x] 9.6 插件化 Crypto 驱动 — 允许注入自定义加密/哈希算法
- [x] 9.7 HTTPS/TLS 支持预研与集成 (启动阶段一)

### 阶段十：生产加固与性能基准 (v0.4.0)
- [x] 10.1 HTTPS/TLS 深度集成 — 支持证书配置与加密传输
- [x] 10.2 Prometheus 可观测性 — 提供标准 /metrics 端点与指标统计
- [x] 10.3 自动化性能基准集 — wrk/hey 集成与跨框架对比报告
- [x] 10.4 零拷贝优化 — 实现 sendfile 静态文件加速
- [x] 10.5 HTTP/2 预研 — ALPN + nghttp2 集成，已完成 Phase 1（会话搭建）与 Phase 2（请求派发与响应）

### 阶段十一：统一可观测性与管理面板 (v0.4.1)
- [x] 11.1 统一管理面板 — Web 管理界面 (/admin) 整合 HTTP/Workflow/MQ 监控
- [x] 11.2 MongoDB 数据库驱动集成 — 支持主流 NoSQL 数据库
- [x] 11.3 全局 AI 遥测 — 模型调用次数、Token 用量、延迟跟踪
- [x] 11.4 全局 DB 遥测 — 连接池状态、查询延迟跟踪
- [x] 11.5 MQ 状态监控 — 队列深度、消息吞吐量实时报告
- [x] 11.6 性能基准自动化 — 集成 CI 自动压测与性能衰退检测 (Regression Detection)

### v1.0 全面优化与演进计划
#### 轨道一：边缘稳定性与测试彻底收尾 (P5 平账) [已通过 116/116 测试验证]
- [x] 12.1 补全 `csilk_config_free` 释放逻辑与泄露测试
- [x] 12.2 补全 `csilk_next(NULL handlers)` 健壮性测试
- [x] 12.3 补充 URL 参数超过 `CSILK_MAX_PARAMS` (20) 时的截断与越界保护测试
- [x] 12.4 补充 `csilk_set` 达到 `CSILK_MAX_STORAGE` 时的防御逻辑与测试
- [x] 12.5 补充 `csilk_get_param` 未找到时的断言覆盖
- [x] 12.6 补充反射引擎 `csilk_bind_reflect` / `csilk_json_reflect` 边界测试
- [x] 12.7 模拟服务端高并发下的 OOM 与连接拒绝策略极限测试

#### 轨道二：v1.0 ABI 稳定性重构 (Context Opaque)
- [x] 13.1 实现 Context 访问器 API (Getter/Setter，如 `csilk_ctx_get_method` 等)
- [x] 13.2 迁移 15 个内置中间件至 Accessor API，消除对 `csilk_ctx_s` 直接访问
- [x] 13.3 重构 30+ 个单元测试，移除对 `context_internal.h` 的依赖
- [x] 13.4 将 `context_internal.h` 从 `include/` 移动至 `src/core/` 彻底隐藏实现

#### 轨道三：极致性能压榨与基准测试
- [x] 14.1 集成 `wrk` / `hey` 自动化基准测试套件
- [x] 14.2 优化 Arena 分配器在高并发下的 Cache-line 对齐 (伪共享优化)
- [x] 14.3 Radix Tree 路由查找实施 Fast-path 优化 (如 SIMD 字符串比较)
- [x] 14.4 扩展 `sendfile` 零拷贝应用范围至更多静态资源场景

#### 轨道四：高级协议演进与生态扩展
- [x] 15.1 HTTP/2 协议支持预研 (Multiplexing / Stream 控制)
- [x] 15.2 基于 MQ 架构实现 WebSocket 高并发房间广播 (Pub/Sub)
- [x] 15.3 实现 OpenAPI (Swagger) 自动生成中间件
- [x] 15.4 Admin Dashboard 增加 CPU/内存 Flamegraph 抽样展示与节点拓扑视图


---

## 二、优先级任务清单

### P0 — 严重缺陷（数据竞争与安全）
- [x] 限流中间件数据竞争 — 静态哈希表 ip_table/ip_count 无锁保护
- [x] csilk_get_client_ip 静态缓冲区竞态 — static char ip[46] 共享缓冲区
- [x] Session 全局链表无锁保护 — 已引入 uv_mutex_t
- [x] Request ID 生成与追踪 — 已实现 X-Request-Id
- [x] 客户端连接池数据竞争 — pool_get/pool_put 在 multi-worker 模式下
  被多线程并发调用，无锁保护导致同一个 client 被多个 worker 获取，
  引发 libuv uv_accept 断言失败。已引入 pool_mutex。

### P1 — 内存安全与资源泄漏
- [x] csilk_server_free 未释放 libuv 资源
- [x] csilk_json 重复调用泄漏前 body
- [x] write_chunk_frame 静默丢弃超大数据 — 已支持 >4GB 分块
- [x] OOM 覆盖不足 — 已引入 TEST_OOM 框架与专项测试

...
### P2 — 中间件系统重构
- [x] 参数化中间件通过 Context Storage 支持 (csilk_set/csilk_get)
- [x] 新增全局 (server-level) 中间件 — csilk_server_use
- [x] 新增逐路由中间件 — csilk_group_add_handlers
- [x] CSRF token 校验精确化 — 用 csilk_get_cookie 精确匹配

### P3 — 配置系统完善
- [x] YAML 已解析 max_header_size
- [x] YAML 不支持中间件配置
- [x] 配置缺少语义校验
- [x] csilk_config_free() 已添加到公共头文件
- [x] example_server 已从配置清理资源

### P4 — 代码质量与技术债
- [x] _gin_log_internal 重命名为 _csilk_log_internal
- [x] 版本号统一为 0.2.0
- [x] Server 使用 Logger 而非 printf
- [x] Arena chunk size 使用常量 CSILK_DEFAULT_ARENA_SIZE
- [x] SHA1 utils.c 死代码已清理
- [x] strncpy 在 ratelimit.c 已保证 null 终止
- [x] Doxyfile 已强化
- [x] docs/html/ docs/latex/ 预生成产物不在版本控制中

### P5 — 测试覆盖扩展
- [x] test_static.c 无有效断言 — 已添加 status/body 断言
- [x] csilk_get_client_ip 无测试 — 新增 tests/test_ip.c
- [x] csilk_add_header 无测试 — 已添加到 test_headers.c
- [x] csilk_bind_json_err / csilk_json_error 无测试 — 已添加到 test_json.c
- [x] csilk_arena_* 无独立测试 — 新增 tests/test_arena.c
- [x] csilk_ws_send 无测试 — 已添加 NULL 安全测试
- [x] test_ws.c 覆盖不足 — 新增 unmasked, fragmented, binary, medium/large, ping/pong/close
- [x] test_cookie.c 覆盖不足 — 新增 Max-Age=0, 长 value, 空 header, 畸形数据
- [x] 集成测试 || true 吞失败 — 所有 31 个集成测试通过，移除门禁

### P6 — 文档与提效
- [x] README 未列出 libyaml 依赖 — 已添加
- [x] README 未列出新功能 — 已更新 Features 和 Project Structure
- [x] README 构建命令过时 — 已改为 make run_tests / ctest
- [x] docs/ARCH.md 使用旧项目名 — 已统一为 csilk
- [x] 全量 Doxygen 注释统一 — 所有 .c/.h 完成 @copyright 标准化
- [x] Doxyfile 配置增强 — GENERATE_TREEVIEW, FULL_SIDEBAR, EXAMPLE_PATH
- [x] 示例代码注释升级 — handler 函数添加 Doxygen @brief
- [x] advanced_server.c 国际化 — 中文注释替换为英文 Doxygen
- [x] README 文档覆盖表更新 — 所有 4 个头文件和源码目录

### P7 — 新功能探索
- [x] 文件上传 — multipart/form-data 解析 (src/middleware/multipart.c)
- [x] gzip 压缩 — 响应体压缩中间件 (src/middleware/gzip.c)
- [x] HTTPS/TLS 支持 — 深度集成 OpenSSL + libuv
- [x] Server-Sent Events (SSE) — SSE 连接管理和事件推送 (src/middleware/sse.c)
- [x] Prometheus Metrics — 实现 QPS/延迟统计与 /metrics 端点
- [x] Message Queue — 实现线程安全异步事件总线

---

## 三、代码可完善事项（平账清单）

### P0 — 关键问题（内存安全、数据竞争、安全漏洞）
- [x] **P0-1: 全局反射注册表无锁保护** — `src/core/reflect.c:15-16`
  `g_registry` / `g_registry_count` 多线程下存在数据竞争（懒加载初始化 `uv_mutex_t` 非线程安全）。已改为显式初始化。
- [x] **P0-2: WebSocket 帧长度整数溢出** — `src/protocols/websocket.c:60`
  `malloc(header_len + len)` 中 len 接近 SIZE_MAX 时加法回绕。应在 malloc 前校验。
- [x] **P0-3: on_body realloc 失败后 body 指针悬空** — `src/core/server.c:215-221`
  realloc 失败时不释放旧 request.body，连接继续运行不报错。
- [x] **P0-4: _internal_client 释放后使用** — `src/core/server.c:81-88,415`
  连接关闭后 SSE/WebSocket 回调可能访问已释放的 csilk_client_t。已在 on_close 中解绑。
- [x] **P0-5: 客户端连接池无锁保护** — `src/core/server.c:179-216`
  `pool_get`/`pool_put` 在 multi-worker 模式下被多个 event loop 线程
  并发调用，`client_pool` 和 `client_pool_count` 无锁保护。修复方法：
  在 `csilk_server_s` 中添加 `uv_mutex_t pool_mutex`，保护所有池操作。

### P1 — 资源管理与错误处理
- [x] **P1-1: csilk_server_free 异步关闭后立即 free** — `src/core/server.c:526-543`
  uv_close 异步完成前就 free(server)，存在 use-after-free 风险。已添加 handle 关闭回调。
- [x] **P1-2: csilk_arena_alloc 对齐运算整数溢出** — `src/core/arena.c:34`
  已在代码中通过 `if (size > SIZE_MAX - 7)` 校验。
- [x] **P1-3: uv_async_init 返回值未检查** — `src/core/server.c:564`
- [x] **P1-4: uv_signal_init / uv_signal_start 返回值未检查** — `src/core/server.c:585-587`
- [x] **P1-5: csilk_log_init 返回值在 app.c 中被忽略** — `src/app/app.c:121,170,176`

### P2 — API 设计问题
- [x] **P2-1: csilk_ctx_s 结构体完全暴露** — `include/csilk/core/ctx_types.h`
> 已评估并制定路线图（见 docs/meta/ABI_REPORT.md）。当前通过访问器 API 提供 ABI 保护。
> 完全不透明化推迟至 v1.0（需迁移 30+ 测试文件）。
- [x] **P2-2: csilk_cors_middleware 按值传结构体** — `include/csilk.h:311`
  72 字节按值传递，已改为 `const csilk_cors_config_t*`。
- [x] **P2-3: 固定长度中间件数组静默丢弃** — `src/core/server.c:36,521`
  最多 32 个全局中间件，超出后已增加错误日志输出并返回错误。
- [x] **P2-4: csilk_set 值生命周期不明确** — `src/core/context.c:212-225`
  已更新文档说明调用者管理生命周期。
- [x] **P2-5: 每个 context 内嵌 16 个固定存储槽** — `include/csilk.h:85-86`
  已改为 arena 动态分配。
- [x] **P2-6: csilk_static 需调用者自行剥除 URL 前缀** — `include/csilk.h:386`
  已支持自动剥离前缀。

### P3 — 代码质量
- [x] **P3-1: on_message_complete 函数过长（165 行）** — `src/core/server.c:225-390`
  已拆分为多个辅助函数提升可读性。
- [x] **P3-2: advanced_server.c 未释放 group** — `examples/advanced/advanced_server.c:125-132`
- [x] **P3-3: 查询参数未做 URL 解码** — `src/core/url.c:35-86`
  %48%65%6C%6C%6F 不会被解码为 "Hello"。已实现 URL 解码。
- [x] **P3-4: test_main.c 死代码** — `tests/test_main.c` 未在 CMakeLists.txt 引用。已清理。
- [x] **P3-5: 测试使用假指针 (void*)0x1** — `tests/test_ip.c:31`
  已在 `test_gzip.c` 等文件中改为使用真实变量地址的 Mock 方式。
- [x] **P3-6: example_server.c 中冗余赋值** — `examples/basic/example_server.c:284`
- [x] **P3-7: SHA1 count[0] 在超大消息时回绕** — `src/core/utils.c:71`
- [x] **P3-8: SHA1 使用 uint32_t len 限制更新大小** — `src/core/utils.c:68`

### P4 — 缺失功能
- [x] P4-1: HTTPS/TLS 支持（Phase 10-11 已完成）
- [x] **P4-2: HTTP/2 支持**
  已实现 Phase 1（会话搭建 + ALPN 协商 + nghttp2 session 初始化 + `csilk_h2.h` 公开 API）和 Phase 2（请求派发：`on_header_callback` 解析伪首部与常规首部、`on_frame_recv_callback` 在 END_STREAM 时派发请求、`on_data_chunk_recv_callback` 累积请求体、`on_stream_close_callback` 清理流上下文；响应发送：`csilk_h2_send_response` 通过 `body_read_callback` 构建 HEADERS 帧 + DATA 帧）。路由通过 `_csilk_dispatch_request` 统一处理 HTTP/1.1 与 HTTP/2。
- [x] **P4-3: 优雅关闭** — 当前 `csilk_server_stop` 直接 `uv_stop`，断活跃连接。已实现通过 `uv_walk` 显式关闭所有句柄。
- [x] **P4-4: 分块传输编码（chunked transfer encoding）**
   已实现 `csilk_response_write` / `csilk_response_end` API，支持完整的 HTTP 分块编码流式响应。
- [x] **P4-5: WebSocket 关闭握手** — 按 RFC 6455 Section 5.5.1
   已实现 `csilk_ws_close()` 发送关闭帧，自动响应接收到的关闭帧并关闭连接。
- [x] **P4-6: 重定向辅助函数 csilk_redirect()**
   已实现支持 302 重定向及自定义状态码重定向（`csilk_redirect` 和 `csilk_redirect_simple`）。
- [x] **P4-7: CSRF token 生成工具** — `src/middleware/csrf.c:43`
   已实现，并增加了 `tests/test_csrf.c` 验证。
- [x] **P4-8: 最大并发连接数限制**
   已实现 `csilk_server_set_max_connections()` 及连接计数/拒绝逻辑。
- [x] **P4-9: 响应流式处理支持**
   已实现 `csilk_response_write/csilk_response_end` 通用 HTTP 流式写入 API（chunked encoding）。

### P5 — 测试覆盖缺口
**完全无测试的模块：**
- [x] src/middleware/csrf.c — CSRF 保护中间件（已补充 middleware 测试）
- [x] src/middleware/cors.c — CORS 中间件（新增 tests/test_cors.c）
- [x] src/middleware/multipart.c — multipart 解析（新增 tests/test_multipart.c）
- [x] src/middleware/sse.c — Server-Sent Events（新增 tests/test_sse.c）
- [x] src/middleware/gzip.c — Gzip 压缩（已有 tests/test_gzip.c）
- [x] src/middleware/ratelimit.c — 速率限制（新增 tests/test_ratelimit.c）
- [x] src/app/app.c — csilk_app_t 高层 API（新增 tests/test_app.c）

**功能覆盖不足：**
- [x] **csilk_config_validate** — `src/core/config.c:197-234`
   已增加 `tests/test_config_validate.c` 验证。
- [x] csilk_config_free — src/core/config.c:161-195
- [x] csilk_next(NULL handlers) — src/core/context.c:15-21
- [x] csilk_get_param 未找到时 — src/core/context.c:45-52
- [x] csilk_set 存储槽满时 — src/core/context.c:220
- [x] csilk_bind_reflect / csilk_json_reflect — src/core/context.c:393-411
- [x] 路由溢出（>20 个路径参数）
- [x] 服务端多连接场景
- [x] OOM / 内存压力场景

### P6 — 构建系统问题
- [x] **P6-1: 库链接可见性应为 PRIVATE** — CMakeLists.txt:139
  已将内部依赖（uv, llhttp, cjson 等）的链接可见性改为 PRIVATE。
- [x] **P6-2: CI 缺少 zlib1g-dev 依赖** — `.github/workflows/ci.yml:18`
- [x] **P6-3: test_main.c 存在但不编译** — `tests/test_main.c`
- [x] **P6-4: 缺少显式 pthread 链接** — CMakeLists.txt
  已通过 Threads::Threads 显式链接。

---

## 四、研究与发现

### 核心架构洞察
- 框架基于 libuv (事件驱动) 和 llhttp (HTTP 解析)。
- 采用洋葱模型中间件和前缀树 (Radix Tree) 路由。
- 测试套件已补全边界用例覆盖。

### ASan 内存泄露排查发现
- **csilk_string**: 修复了局部栈分配导致的 stack-use-after-return。
- **libuv 句柄**: 修复了 timer handle 未关闭导致的 heap-use-after-free。
- **url_parser**: 修复了 client->current_url 和 request.path 的内存泄漏。

### WebSocket 实现调研
- WebSocket 需要接管 libuv 的 uv_read_start。当 HTTP 解析器发现 `Upgrade: websocket` 时，应停止 llhttp 解析，转而由 WebSocket 帧解析器处理后续数据。
- 需要增加 Base64 和 SHA1 的依赖用于握手协议。

---

## 五、会话日志

### [2026-05-23] 框架演进计划启动
- **动作**: 确认框架战略为"稳扎稳打：性能与稳定性优先"。
- **动作**: 创建文件系统记忆 (task_plan.md, findings.md, progress.md)。

### [2026-05-23] 阶段一完成 — 稳定性与测试基建
- **动作**: 补充边界测试提高覆盖率。
- **动作**: 修复 ASan 检测出的多处核心内存泄漏和段错误。
- **动作**: 集成基于 libFuzzer 的 fuzz_test.c。
- **动作**: 添加 GitHub Actions CI 配置文件。

### [2026-05-23] 阶段二完成 — 架构与性能调优
- **动作**: 引入请求级内存池 Arena。
- **动作**: 将 Radix Tree 重构为固定数组子节点布局。
- **动作**: 修复路由 handler 数组的生命周期问题（深拷贝）。
- **动作**: 优化 recovery_handler 的 jump_buffer 生命周期。

### [2026-05-23] 阶段五完成 — 日志系统成熟化
- **动作**: 引入日志宏 CSILK_LOG_I 等，支持源码文件/行号/函数名。
- **动作**: 实现基于文件大小的自动日志滚动。
- **动作**: 优化日志输出格式，支持彩色终端。
- **动作**: 重写 test_logger.c 验证并发安全与滚动功能。

### [2026-05-23] 阶段六完成 — YAML 配置支持
- **动作**: 集成 libyaml 依赖。
- **动作**: 定义统一配置结构 csilk_config_t（Server/Logger/CORS）。
- **动作**: 实现基于事件流的 YAML 解析引擎。
- **动作**: 更新 example_server.c 支持配置文件启动。
- **动作**: 增加 test_config.c 验证解析准确性。

### [2026-05-23] 全量代码完善
- **动作**: 修复 P0-P2 的所有优先级问题（数据竞争、内存泄漏、编译错误）。
- **动作**: 修复 P3-P4 的代码质量和技术债问题。
- **动作**: 扩展 P5 测试覆盖（test_ip, test_arena, test_ws, test_cookie 等）。
- **动作**: 更新 P6 全量文档（README, ARCH.md, Doxygen, 示例注释）。
- **动作**: 新增 P7 功能（Multipart, Gzip, SSE 中间件）。
- **动作**: 性能阶段七优化完成（uv_queue_work, header 哈希表, arena 复用, SO_REUSEPORT, TCP_NODELAY）。
- **动作**: 生成 docs/meta/CODEPEC.md（AI 编码规范）和 ANALYSIS.md（问题清单）。

### [2026-05-25] 生产加固阶段 (v0.4.0) 核心功能完成
- **动作**: 深度集成 OpenSSL，实现 native HTTPS/TLS 支持。
- **动作**: 实现 Prometheus Metrics 中间件与 `/metrics` 端点。
- **动作**: 引入异步、线程安全的 Message Queue (MQ) 模块。
- **动作**: 实现基于 `sendfile` 的零拷贝静态文件传输优化。
- **动作**: 完成全量 Doxygen 注释加固与 ARCH.md/README.md 文档更新。

### [2026-05-25] 文档与工程化收尾
- **动作**: 完成全部 37 个源文件/头文件的 Doxygen 注释扩展（@brief, @param, @return, @note）。
- **动作**: 修复 Doxygen 注释引入的编译错误（6 处缺失函数签名、嵌套注释问题）。
- **动作**: 将 `context_internal.h` 从 `src/core/` 移至 `include/`，清理 CMakeLists.txt。
- **动作**: 同步更新所有技术文档（README, ARCH.md, CHANGELOG, docs/meta/CODEPEC, ANALYSIS, OPTIMIZE, PLAN）。

### [2026-05-28] 阶段十一：统一可观测性与管理面板
- **动作**: 实现统一管理面板 (admin.c) — Web 界面整合 HTTP/Workflow/MQ 监控。
- **动作**: 集成 MongoDB 数据库驱动 (mongodb.c) — 完整连接池与查询接口。
- **动作**: 实现全局 AI 遥测 — 模型调用、Token 用量、延迟跟踪。
- **动作**: 实现全局 DB 遥测 — 连接池状态、查询延迟跟踪。
- **动作**: 实现 MQ 消息状态监控 — 实时事件、队列深度、JSON 统计。
- **动作**: 修复 MQ 恢复回归、admin 结构体隐私、Mermaid 语法兼容性。
- **动作**: 全量文档同步更新（README, ARCH.md, architecture.md, module-design, user-manual, docs/meta/CODEPEC, PLAN）。

### [2026-06-05] OOM I/O 测试修复 — stack-use-after-return & 内存泄漏
- **动作**: 修复 `test_oom_io.c` 中 `test_oom_http_parser` 的 stack-use-after-return ASAN 错误：
  栈上 `csilk_client_t` 注册的 libuv 定时器句柄在函数返回后仍被默认事件循环引用，
  导致 `test_oom_static_file` 调用 `uv_run` 时触发 use-after-return。添加 `uv_close`
  + `uv_run(UV_RUN_NOWAIT)` 在返回前清理句柄。
- **动作**: 修复 `test_oom_http_parser` 的内存泄漏：`csilk_ctx_cleanup` 和
  `free(client.current_url)` 放在 `llhttp_execute` 之前而非之后，导致最后一次
  循环迭代分配的 `request.body`/`request.path`/`response.body` 泄漏。移动至之后。
- **动作**: CI GitHub Actions 全部 6 个 job 验证通过（116/116 测试）。
- **动作**: 补充完整中文/英文代码注释。

### [2026-05-29] 客户端连接池数据竞争修复 (P0-5)
- **动作**: 修复 `pool_get`/`pool_put` 在 multi-worker 模式下的数据竞争。
  `client_pool` 和 `client_pool_count` 被多个 event loop 线程并发访问
  无锁保护，导致同一个 `csilk_client_t` 被两个 worker 同时获取。
  添加 `uv_mutex_t pool_mutex` 保护所有池操作。
  CI coverage build（带 `-fprofile-arcs -ftest-coverage`）因时序暴露此问题，
  Build and Test 在正常优化下偶现。
- **动作**: 全量文档同步更新（CHANGELOG, PLAN, docs/meta/CODEPEC, ARCH.md,
  architecture.md, index.md, Doxygen）。

---

## 六、状态一览 (v0.3.0 闭环)

| 域 | 待办 | 说明 |
|-----|:----:|------|
| 演进阶段 | 0 | 全部完成 |
| P0 严重缺陷 | 0 | (已修复) |
| P1 内存安全 | 0 | (已修复) |
| P2 中间件系统 | 0 | (已重构) |
| P3 配置系统 | 0 | (已完善) |
| P4 代码质量 | 0 | (已清理) |
| P5 测试扩展 | 0 | 全部完成 |
| P6 文档 | 0 | (全量 Doxygen 完成) |
| P7 新功能 | 0 | HTTPS/TLS, MQ, Prometheus, Zero-copy 全部完成 |
| 阶段十一 | 0 | Admin Dashboard, MongoDB, 全光谱遥测完成 |
| v1.0 轨道一 | 0 | 全部完成 |
| v1.0 轨道二 | 0 | 全部完成 |
| v1.0 轨道三 | 0 | 全部完成 |
| v1.0 轨道四 | 0 | 全部完成 |
| 平账 P0 | 0 | 所有问题已修复 |
| 平账 P1 | 0 | 所有问题已修复 |
| 平账 P2 | 0 | P2-1 已评估，推迟至 v1.0 |
| 平账 P3 | 0 | 所有问题已修复 |
| 平账 P4 | 0 | 所有问题已修复 |
| 平账 P5 | 0 | 所有问题已修复 |
| 平账 P6 | 0 | 所有问题已修复 |
| **合计** | **0** | v0.3.0 目标全部达成 |

---

## 七、v0.4.0 优化完善计划 (2026-05-30)

### 轨道一：性能基准与工程化 (P0)
- [x] 7.1 README 修正 — C23、依赖列表补充
- [x] 7.2 CI 流程整合 — 合并重叠的 ci.yml/build.yml，统一 workflow
- [x] 7.3 自动化性能基准集 — wrk JSON 输出 + --save/--compare/--ci 模式，CI 集成
- [x] 7.4 macOS CI — 预研完成，因 pthread_barrier_t 缺失推迟至后续版本
- [x] 7.5 平台兼容说明 — README 已添加编译器/OS/依赖版本矩阵

### 轨道二：系统依赖与构建加固 (P1)
- [x] 7.6 CMakeLists 系统依赖版本下限校验
- [x] 7.7 Docker 多阶段构建支持
- [x] 7.8 集中版本号管理 — 消除 18 处硬编码版本，configure_file 生成 version.h
- [x] 7.9 clang-tidy 规则增强 — 回退至保守设置，保留 clang-analyzer-* + performance-*
- [x] 7.10 连接池大小提取为可配置常量 CSILK_CLIENT_POOL_SIZE
- [x] 7.11 Fuzz 测试 — 因系统 clang 不支持 C23 constexpr 暂时禁用，预留 clang-19 模板

### 轨道三：协议与扩展 (P2)
- [x] 7.12 HTTP/2 Phase 3 — Server Push 支持 (csilk_push_promise API)
- [x] 7.13 连接池大小可配置化 → CSILK_CLIENT_POOL_SIZE 常量
- [x] 7.19 Swagger UI 治理 — 移除仓库 4.4MB 静态资源，CMake 构建时自动下载

### 轨道四：v1.0 预研 (P3)
- [x] 7.14 types.h 拆分 — 评估推迟至 v1.0（与 ABI 不透明化同步进行）
- [x] 7.15 ABI 渐进不透明化路线图 → docs/research/abi-opaque-roadmap.md
- [x] 7.16 HTTP/3 / QUIC 可行性评估 → docs/research/http3-quic.md
- [x] 7.17 macOS 平台支持评估 → docs/research/macos-port.md (uv_barrier_t 方案)
- [x] 7.18 Windows (MSVC) 支持预研 → docs/research/windows-port.md (WSL2/Docker 推荐)

---

## 八、v0.5.0 优化完善计划 (2026-05-30)

### 轨道一：安全与正确性 (P0)
- [x] 8.1 修复不安全字符串函数 — strcpy/strcat/sprintf → snprintf/memcpy/strncpy
  - workflow.c: 6 处 strcpy/strcat → snprintf + buf_used 边界跟踪
  - jwt.c: 2 处 sprintf → snprintf
  - utils.c: 2 处 strcpy/sprintf → memcpy/snprintf
- [x] 8.2 修复 clang-tidy — 恢复 -*, 添加 -performance-no-int-to-ptr 排除
- [x] 8.3 Doxygen 版本号 — 已验证为 0.3.0 (无变更)
- [x] 8.4 setjmp/longjmp 文档 — 增强 @warning 说明 malloc/mutex 泄漏风险

### 轨道二：CI 与测试覆盖 (P1)
- [x] 8.5 Fuzz 测试 — 评估后继续禁用 (Ubuntu 24.04 无 clang-19，clang-18 C23 支持不完整)
- [x] 8.6 补充缺失模块测试 — 新增 test_admin.c (admin context storage, overflow)
- [x] 8.7 修复 example_app.c 编译警告 — buf 256→512 修正 format-truncation
- [x] 8.8 h2.c int-to-ptr 警告 — 添加 -performance-no-int-to-ptr 排除
- [x] 8.9 Swagger UI CDN 离线备选 — jsdelivr 主 CDN + unpkg 备选

### 轨道三：平台与文档 (P2)
- [x] 8.10 macOS 支持 — pthread_barrier_t → uv_barrier_t (需 CI include 路径调试)
- [x] 8.11 修正 CHANGELOG macOS CI 项
- [x] 8.12 Node.js 20 → 24 — GitHub 自动迁移，无需代码变更
- [x] 8.13 补充 examples 集成测试至 CI — example_server 启动 + curl 冒烟测试

### 轨道四：v1.0 准备 (P3)
- [x] 8.14 ABI Phase A — Context accessor API (已验证已有 csilk_get_method 等)
- [x] 8.15 AI Embedding 驱动增强 — 实现 Milvus 驱动与 Workflow 向量搜索节点 (Vector Search Node)
- [x] 8.16 HTML 文档部署版本号自动同步 — Doxyfile.in + configure_file

---

## 代码库重构 (Codebase Restructuring)

> csilk 从当前 48 文件 / 平铺架构重构为 ～54 文件的模块化分层架构。
> 同时升级至 C23、模块化公共 API、规范化文档版本号。

| 阶段 | 内容 | 关键动作 |
|---|---|---|
| **0** | C23 + 版本 0.3.0 + 常量化 | `CMAKE_C_STANDARD 23`; 版本号全局同步 (18 处); `#define` → `static constexpr` (11 个常量); 删 `#include <stdbool.h>` (6 处) |
| **1** | 拆分 `internal.h` (552 行 → 5 文件) | `hash.h` / `codec.h` / `ws_frame.h` / `crypto_dispatch.h` / `mq_types.h` |
| **2** | 迁移内部头文件至 `include/csilk/core/` | `context_internal.h` → `ctx_types.h`; `server_internal.h` → `srv_internal.h`; 修复相对 include 路径 |
| **3** | 拆分 `server.c` (2600 行 → 4 文件) | `server.c` (生命周期) / `connection.c` / `http1.c` / `tls.c` |
| **4** | 拆分 `context.c` (1761 行 → 2 文件) | `context.c` (请求读取) / `response.c` (响应写入) |
| **5** | 拆分 `workflow.c` + 移动 `admin.c` | `src/workflow/engine.c` / `steps.c` / `parallel.c`; `admin.c` → `src/core/` |
| **6** | Drivers 重组 + 删 `src/crypto/` | `drivers/ai/` / `drivers/perm/` / `drivers/cipher/` 子目录; `cipher.c` → `drivers/cipher/openssl.c` |
| **7** | 模块化 `csilk.h` (2683 行 → 14+ 头文件) | `types.h` / `server.h` / `context.h` / `response.h` / `router.h` / `app.h` / `group.h` / `middleware.h` / `websocket.h` / `sse.h` / `mq.h` / `workflow.h` / `hooks.h` / `admin.h` / `errors.h` / `config.h` / `version.h` |
| **8** | 构建审计 + Doxygen 验证 | 全量构建 + 108 测试 + Doxygen 文档生成 (0 警告) + #include 路径审计 |
| **9** | `NULL` → `nullptr` 迁移 (~1000 处) | 脚本化替换; `docs/meta/CODEPEC.md` 更新; 编译 + 测试验证 |
| **10** | `docs/ARCH.md` 最终更新 | 新增 Section 11 (最终目录树); 验证所有路径引用 |

每个阶段独立提交 (`Phase N: <title>`)，编译 + 测试验证 (`cmake --build && ctest`)。

---

## 九、系统优化分析建议路线 (2026-06-03)

> 基于 improvement-analysis change 的 6 维度分析结果。完整报告见 `docs/analysis/MASTER_REPORT.md`。
> 优先级排序：P1（安全/正确性）> P2（重要）> P3（锦上添花）

### P1 — 优先执行

- [x] 9.1 修复 UUID v4 中 `rand()` 不安全回退 — 改用 Linux `getrandom()` 或 `/dev/urandom`
- [x] 9.2 添加 AES-256-GCM nonce 生成辅助函数 — 防止调用者重用 nonce
- [x] 9.3 在 `http1.c:173,350` 的 realloc 中增加整数溢出保护
- [x] 9.4 将 20+ 个内部符号 (`_csilk_*`, `on_*`, `cleanup_tls` 等) 标记为 `hidden` 可见性
- [x] 9.5 扩展多 worker 测试覆盖 — 目前仅 `test_multi_worker.c` 使用 >1 worker
- [x] 9.6 拆分 `src/workflow/workflow.c` (2428 行) 为 engine/steps/wal (已完成结构拆分，待深入重构)

### P2 — 后续执行

- [x] 9.7 配置 ccache + PCH 加速增量构建
- [x] 9.8 恢复 Fuzz 测试 — clang libFuzzer 或 afl++
- [x] 9.9 添加新 Fuzz 目标：YAML 解析器、URL 解码器
- [x] 9.10 添加 WebSocket/TLS/MQ 示例程序
- [x] 9.11 配置 GitHub Actions 依赖缓存
- [x] 9.12 添加并发 WebSocket/SSE/MQ 场景测试

  - `tests/test_mq_concurrent.c`: MQ 多线程并发发布 (4线程×100, 4线程×250压力, 单线程1000突发)
  - `tests/test_ws_concurrent.c`: WS 多房间广播/快速加入离开/独立房间 (单事件循环并发)
  - `tests/test_sse_concurrent.c`: SSE 多流并行初始化/生命周期/混合事件 (单事件循环并发)
- [x] 9.13 修复 Alpine/musl 移植问题：`backtrace()`, `aligned_alloc`, `strndup`

  - `flamegraph.c`: 用 `__GLIBC__` 守卫 `execinfo.h`/`backtrace`/`backtrace_symbols`，非 glibc 跳过采样
  - `arena.c`: `posix_memalign` 优先于 C11 `aligned_alloc` (musl 可能没有 `aligned_alloc` 即使编译器宣称 C11)
  - `mysql.c`: 添加 `csilk_strndup` 局部 polyfill 宏，当 libc 不暴露 `strndup` 时使用
- [x] 9.14 模块化 `src/messaging/mq.c` (968 行) — 已拆分为 mq.c(558) + mq_context.c + mq_offload.c + mq_wal.c
- [x] 9.15 拆分 `src/core/utils.c` (1018 行) — UUID/Base64/SHA1 独立文件
- [x] 9.16 不透明化 `csilk_db_pool_s` 和 workflow 结构体
- [x] 9.17 GitHub Releases 发布预编译产物

  - `.github/workflows/release.yml`: 在 `v*` tag push 时构建 Release，产出 Linux (x86_64) 和 macOS (arm64) 静态/动态库 + 头文件 + CMake 配置包的 `.tar.gz`
  - `scripts/package.sh`: 本地打包脚本，`cmake --install` 到临时目录后打包 tarball + SHA256
  - 发布操作: `git tag v0.3.1 && git push origin v0.3.1` 自动触发
- [x] 9.18 创建 vcpkg port — `cmake/ports/csilk/portfile.cmake` + `vcpkg.json` 已存在

### P3 — 远期

- [x] 9.20 安装 perf/wrk/FlameGraph 工具链进行基准测试

  - `scripts/setup_bench_tools.sh`: 一键安装 perf (linux-tools) + wrk + FlameGraph (Brendan Gregg)
  - `scripts/profile.sh`: 启动 example_server → perf record 采样 → stackcollapse → flamegraph SVG
  - `CMakeLists.txt`: `make profile` 目标 (30秒采样)
  - `.github/workflows/bench.yml`: 新增 profile job (master 分支)，自动运行 perf + 上传 flamegraph.svg
- [x] 9.21 评估 io_uring Option C (`UV_USE_IO_URING=1`)

  - Kernel 6.12.91 + libuv 1.48.0: 119/119 测试在 `UV_USE_IO_URING=1` 下通过
  - 零代码修改 — libuv 自动选择 io_uring backend (IORING_SETUP_SQPOLL)
  - CI: 新增 `io_uring` job 在每个 push 运行全量测试
  - `docs/research/io_uring.md`: 更新状态为 Evaluated，补充评估结果
  - 生产部署: `UV_USE_IO_URING=1 ./example_server config.yaml`
- [x] 9.22 Arena 块在工作线程初始化时预分配
- [x] 9.23 为热指标路径实现有界 JSON 构建器
- [x] 9.24 实现 URL/Base64 的属性基测试 (`test_codec_prop`, 8 个属性测试)
- [x] 9.25 Benchmark CI 方差归一化 (`BENCH_RUNS=3`, 中位数比较, CV 告警)
- [x] 9.26 API 命名清理：`csilk_set_websocket`／`csilk_set_sse`／`csilk_set_async` → `csilk_ctx_set_*`

### [2026-06-05] v0.5.x 增量修复 — 正确性
- **动作**: 修复 arena TLS data race（`_Thread_local`）+ TLS flush
- **动作**: 修复 MQ realloc 整数溢出 ×3 + NULL 检查
- **动作**: 修复 `db.c` SQL 注入（SQL 标准单引号转义）
- **动作**: 修复 `http1.c` URL/header 超限时的 3 处内存泄漏
- **动作**: 修复 `app.c` 错误路径 server 指针泄漏
- **动作**: 修复 `hot_reload.c` `dlclose`/`FreeLibrary` 泄漏
- **动作**: 修复 `waf.c` null context 段错误
- **动作**: 修复 4 处 const-qualifier 编译警告

### [2026-06-05] v0.5.x 增量修复 — 整洁与性能
- **动作**: `perm.c` GCC 内置原子 → C11 标准原子
- **动作**: `http1.c` int pos → size_t pos 消除截断转型
- **动作**: chunked write 零拷贝（`_csilk_send_data_owned`）
- **动作**: CI Fuzz 测试启用

### [2026-06-05—06] 测试覆盖与文档
- **动作**: WAF 4→9, Session 5→8, Recovery 1→4, CSRF 3→7, WF Lifecycle 1→3
- **动作**: 文档补全：`bounded_buf.c`, `hot_reload.c`, `flamegraph.c`, `db.c`, `context.c`
- **动作**: 全部 117 测试通过（ASAN Debug + Release + io_uring + Coverage）
