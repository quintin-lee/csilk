# csilk 代码分析报告 — 可完善事项

> 生成日期: 2026-05-23 | 基于 commit `a47677c`
> 最后更新: 2026-05-24 | 大部分问题已修复

---

## P0 — 关键问题（内存安全、数据竞争、安全漏洞）

- [x] **P0-1: 全局反射注册表无锁保护** — `src/core/reflect.c:15-16`
  `g_registry` / `g_registry_count` 在多线程下存在数据竞争（懒加载初始化 `uv_mutex_t` 非线程安全）。应改为显式初始化。

- [x] **P0-2: WebSocket 帧长度整数溢出** — `src/core/websocket.c:60`
  `malloc(header_len + len)` 中 `len` 接近 `SIZE_MAX` 时加法回绕，导致堆缓冲区溢出。应在 malloc 前校验 `len <= SIZE_MAX - header_len`。

- [x] **P0-3: `on_body` realloc 失败后 body 指针悬空** — `src/core/server.c:215-221`
  realloc 失败时不释放旧 `request.body`，且连接继续运行不报错。

- [x] **P0-4: `_internal_client` 释放后使用** — `src/core/server.c:81-88, 415`
  连接关闭后 SSE/WebSocket 回调可能访问已释放的 `csilk_client_t`。

---

## P1 — 资源管理与错误处理

- [x] **P1-1: `csilk_server_free` 异步关闭后立即 free** — `src/core/server.c:526-543`
  `uv_close` 异步完成前就 `free(server)`，存在 use-after-free 风险。

- [x] **P1-2: `csilk_arena_alloc` 对齐运算整数溢出** — `src/core/arena.c:34`
  `size + 7` 在 `size` 接近 `SIZE_MAX - 7` 时回绕为小值。

- [x] **P1-3: `uv_async_init` 返回值未检查** — `src/core/server.c:564`
  初始化失败后 `csilk_server_stop` 调用 `uv_async_send` 行为未定义。

- [x] **P1-4: `uv_signal_init` / `uv_signal_start` 返回值未检查** — `src/core/server.c:585-587`
  信号处理静默失败。

- [x] **P1-5: `csilk_log_init` 返回值在 app.c 中被忽略** — `src/app/app.c:121,170,176`
  日志初始化失败（如无法打开文件）时无反馈。

---

## P2 — API 设计问题

- [ ] **P2-1: `csilk_ctx_s` 结构体完全暴露** — `include/csilk.h:71-89`
  ABI 不稳定；私有字段 `_internal_client` 暴露给用户。应改为不透明类型。

- [x] **P2-2: `csilk_cors_middleware` 按值传结构体** — `include/csilk.h:311`
  72 字节 `csilk_cors_config_t` 按值传递，效率低。已改为 `const csilk_cors_config_t*`。

- [x] **P2-3: 固定长度中间件数组静默丢弃** — `src/core/server.c:36,521`
  最多 32 个全局中间件，超出后静默忽略。已改为返回错误码 + 日志输出。

- [x] **P2-4: `csilk_set` 值生命周期不明确** — `src/core/context.c:212-225`
  只 free key 不 free value，文档未说明调用者负责管理 value 生命周期。

- [x] **P2-5: 每个 context 内嵌 16 个固定存储槽** — `include/csilk.h:85-86`
  空槽浪费栈空间；限制为 16 个值。已改为 arena 分配。

- [x] **P2-6: `csilk_static` 需要调用者自行剥除 URL 前缀** — `include/csilk.h:386`
  低层 API 直接使用 `c->request.path`，高层的 `csilk_app_static` 已封装此逻辑，但低层 API 易误用。

---

## P3 — 代码质量

- [x] **P3-1: `on_message_complete` 函数过长（165 行）** — `src/core/server.c:225-390`
  已拆分为 `finalize_request()`、`_csilk_send_response()` 等辅助函数。

- [x] **P3-2: `advanced_server.c` 未释放 group** — `examples/advanced_server.c:125-132`
  `root`、`api` 等 group 创建后从未 free，资源泄漏。

- [x] **P3-3: 查询参数未做 URL 解码** — `src/core/url.c:35-86`
  `%48%65%6C%6C%6F` 不会被解码为 "Hello"，影响依赖查询参数的应用程序。

- [x] **P3-4: `test_main.c` 死代码** — `tests/test_main.c`
  文件存在但未在 CMakeLists.txt 中被引用或编译。

- [x] **P3-5: 测试使用假指针 `(void*)0x1`** — `tests/test_ip.c:31`
  假指针脆弱，实现变更后可能误判或崩溃。

- [x] **P3-6: `example_server.c` 中冗余赋值** — `examples/example_server.c:284`
  `handlers[1] = NULL;` 重复了初始化器的赋值。

- [x] **P3-7: SHA1 `count[0]` 在超大消息时回绕** — `src/core/utils.c:71`
  `uint32_t` 计数器在消息超过 ~4GB 时回绕（WebSocket 握手无影响，但实现不严谨）。

- [x] **P3-8: SHA1 使用 `uint32_t len` 限制更新大小** — `src/core/utils.c:68`
  参数类型应为 `size_t` 以保持可移植性。

---

## P4 — 缺失功能

- [ ] **P4-1: HTTPS/TLS 支持** — 需深度集成 OpenSSL + libuv，PLAN.md 已标注待后续
- [ ] **P4-2: HTTP/2 支持**
- [x] **P4-3: 优雅关闭** — 已实现 `uv_walk` + 显式关闭所有句柄
- [x] **P4-4: 分块传输编码（chunked transfer encoding）** — 已实现 `csilk_response_write/end`
- [x] **P4-5: WebSocket 关闭握手** — 已实现 `csilk_ws_close()` 发送关闭帧
- [x] **P4-6: 重定向辅助函数 `csilk_redirect()`** — 已实现
- [x] **P4-7: CSRF token 生成工具** — 已实现
- [x] **P4-8: 最大并发连接数限制** — 已实现 `csilk_server_set_max_connections()`
- [x] **P4-9: 响应流式处理支持** — 已实现 `csilk_response_write/csilk_response_end`

---

## P5 — 测试覆盖缺口

### 完全无测试的模块：

- [x] `src/middleware/csrf.c` — CSRF 保护中间件（已补充 middleware 测试）
- [x] `src/middleware/cors.c` — CORS 中间件（新增 tests/test_cors.c）
- [x] `src/middleware/multipart.c` — multipart 解析（新增 tests/test_multipart.c）
- [x] `src/middleware/sse.c` — Server-Sent Events（新增 tests/test_sse.c）
- [x] `src/middleware/gzip.c` — Gzip 压缩（已有 tests/test_gzip.c）
- [x] `src/middleware/ratelimit.c` — 速率限制（新增 tests/test_ratelimit.c）
- [x] `src/app/app.c` — `csilk_app_t` 高层 API（新增 tests/test_app.c）

### 功能覆盖不足：

- [x] `csilk_config_validate` — 已增加 `tests/test_config_validate.c`
- [ ] `csilk_config_free` — `src/core/config.c:161-195` 未测试
- [ ] `csilk_next(NULL handlers)` — `src/core/context.c:15-21` 未测试
- [ ] `csilk_get_param` 未找到时 — `src/core/context.c:45-52` 未测试
- [ ] `csilk_set` 存储槽满时 — `src/core/context.c:220` 未测试
- [ ] `csilk_bind_reflect` / `csilk_json_reflect` — `src/core/context.c:393-411` 未直接测试
- [ ] 路由溢出（>20 个路径参数） — `src/core/router.c` 未测试
- [ ] 服务端多连接场景 — `src/core/server.c` 未测试
- [ ] OOM / 内存压力场景 — 全部未测试

---

## P6 — 构建系统问题

- [x] **P6-1: 库链接可见性应为 PRIVATE** — `CMakeLists.txt:139`
  `uv`、`llhttp`、`yaml`、`zlib` 对外泄漏为 PUBLIC，使用者需链接不必要的库。

- [x] **P6-2: CI 缺少 `zlib1g-dev` 依赖** — `.github/workflows/ci.yml:18`
  构建因 runner 预装 zlib 才侥幸通过。

- [x] **P6-3: `test_main.c` 存在但不编译** — `tests/test_main.c`
  死代码应移除或纳入构建。

- [x] **P6-4: 缺少显式 `pthread` 链接** — `CMakeLists.txt`
  多个测试使用 pthread 但未显式链接，依赖 libuv 传递。

---

## 优先级推荐

| 优先级 | 工作项 | 预估难度 |
|--------|--------|----------|
| **已完成** | P0-1 反射注册表加锁 | - |
| **已完成** | P0-2 WebSocket 长度校验 | - |
| **已完成** | P0-3 on_body realloc 失败处理 | - |
| **已完成** | P0-4 _internal_client UAF | - |
| **已完成** | P1-1 server_free UAF | - |
| **已完成** | P1-2 arena 溢出 | - |
| **已完成** | P1-3/4 uv_* 返回值检查 | - |
| **已完成** | P2-2 CORS 按指针传递 | - |
| **已完成** | P3-3 URL 解码 | - |
| **待定** | P2-1 ctx 不透明化 | 高（重构） |
| **低** | P5 测试覆盖边缘项 | 持续 |
