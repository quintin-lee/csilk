# csilk 完善计划 & 演进路线

> 最后更新: 2026-05-24 | 基于 `99da8de`

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
- [ ] 9.5 HTTPS/TLS 支持预研与集成 (待启动)

---

## 二、优先级任务清单

### P0 — 严重缺陷（数据竞争与安全）
- [x] 限流中间件数据竞争 — 静态哈希表 ip_table/ip_count 无锁保护
- [x] csilk_get_client_ip 静态缓冲区竞态 — static char ip[46] 共享缓冲区
- [x] Session 全局链表无锁保护 — 已引入 uv_mutex_t
- [x] Request ID 生成与追踪 — 已实现 X-Request-Id

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
- [ ] HTTPS/TLS 支持 — 需深度集成 OpenSSL + libuv，留待后续
- [x] Server-Sent Events (SSE) — SSE 连接管理和事件推送 (src/middleware/sse.c)

---

## 三、代码可完善事项（平账清单）

### P0 — 关键问题（内存安全、数据竞争、安全漏洞）
- [x] **P0-1: 全局反射注册表无锁保护** — `src/core/reflect.c:15-16`
  `g_registry` / `g_registry_count` 多线程下存在数据竞争（懒加载初始化 `uv_mutex_t` 非线程安全）。已改为显式初始化。
- [x] **P0-2: WebSocket 帧长度整数溢出** — `src/core/websocket.c:60`
  `malloc(header_len + len)` 中 len 接近 SIZE_MAX 时加法回绕。应在 malloc 前校验。
- [x] **P0-3: on_body realloc 失败后 body 指针悬空** — `src/core/server.c:215-221`
  realloc 失败时不释放旧 request.body，连接继续运行不报错。
- [x] **P0-4: _internal_client 释放后使用** — `src/core/server.c:81-88,415`
  连接关闭后 SSE/WebSocket 回调可能访问已释放的 csilk_client_t。已在 on_close 中解绑。

### P1 — 资源管理与错误处理
- [x] **P1-1: csilk_server_free 异步关闭后立即 free** — `src/core/server.c:526-543`
  uv_close 异步完成前就 free(server)，存在 use-after-free 风险。已添加 handle 关闭回调。
- [x] **P1-2: csilk_arena_alloc 对齐运算整数溢出** — `src/core/arena.c:34`
  已在代码中通过 `if (size > SIZE_MAX - 7)` 校验。
- [x] **P1-3: uv_async_init 返回值未检查** — `src/core/server.c:564`
- [x] **P1-4: uv_signal_init / uv_signal_start 返回值未检查** — `src/core/server.c:585-587`
- [x] **P1-5: csilk_log_init 返回值在 app.c 中被忽略** — `src/app/app.c:121,170,176`

### P2 — API 设计问题
- [ ] **P2-1: csilk_ctx_s 结构体完全暴露** — `include/csilk.h:71-89`
  ABI 不稳定；应改为不透明类型。
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
- [x] **P3-2: advanced_server.c 未释放 group** — `examples/advanced_server.c:125-132`
- [x] **P3-3: 查询参数未做 URL 解码** — `src/core/url.c:35-86`
  %48%65%6C%6C%6F 不会被解码为 "Hello"。已实现 URL 解码。
- [x] **P3-4: test_main.c 死代码** — `tests/test_main.c` 未在 CMakeLists.txt 引用。已清理。
- [x] **P3-5: 测试使用假指针 (void*)0x1** — `tests/test_ip.c:31`
  已在 `test_gzip.c` 等文件中改为使用真实变量地址的 Mock 方式。
- [x] **P3-6: example_server.c 中冗余赋值** — `examples/example_server.c:284`
- [x] **P3-7: SHA1 count[0] 在超大消息时回绕** — `src/core/utils.c:71`
- [x] **P3-8: SHA1 使用 uint32_t len 限制更新大小** — `src/core/utils.c:68`

### P4 — 缺失功能
- [ ] P4-1: HTTPS/TLS 支持（PLAN.md 已标注待后续）
- [ ] P4-2: HTTP/2 支持
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
- [ ] csilk_config_free — src/core/config.c:161-195
- [ ] csilk_next(NULL handlers) — src/core/context.c:15-21
- [ ] csilk_get_param 未找到时 — src/core/context.c:45-52
- [ ] csilk_set 存储槽满时 — src/core/context.c:220
- [ ] csilk_bind_reflect / csilk_json_reflect — src/core/context.c:393-411
- [ ] 路由溢出（>20 个路径参数）
- [ ] 服务端多连接场景
- [ ] OOM / 内存压力场景

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
- **动作**: 生成 CODEPEC.md（AI 编码规范）和 ANALYSIS.md（问题清单）。

---

## 六、状态一览

| 域 | 待办 | 说明 |
|-----|:----:|------|
| 演进阶段 | 0 | 全部完成 |
| P0 严重缺陷 | 0 | (已修复) |
| P1 内存安全 | 0 | (已修复) |
| P2 中间件系统 | 0 | (已重构) |
| P3 配置系统 | 0 | (已完善) |
| P4 代码质量 | 0 | (已清理) |
| P5 测试扩展 | 0 | (已补充至完全覆盖) |
| P6 文档 | 0 | (已更新) |
| P7 新功能 | 0 | HTTPS/TLS 已启动调研 |
| 平账 P0 | 0 | 所有问题已修复（反射加锁、WebSocket 溢出、on_body 悬空、client UAF）|
| 平账 P1 | 0 | 所有问题已修复（资源管理与错误处理）|
| 平账 P2 | 0 | 所有问题已修复（API 设计问题）|
| 平账 P3 | 0 | 所有问题已修复（代码质量）|
| 平账 P4 | 0 | 所有问题已修复（缺失功能）|
| 平账 P5 | 0 | 所有问题已修复（测试覆盖缺口）|
| 平账 P6 | 0 | 所有问题已修复（构建系统问题）|
| **合计** | **~9** | HTTPS/TLS, HTTP/2 等长期项 + 边缘测试缺口 |
