# csilk 代码分析报告 — 后续优化完善点

> 生成日期: 2026-05-24 | 基于 commit `90a2d4c`
> 版本: 0.2.1 | 56/56 测试通过，~2.2s

---

## 优先级总览

| 优先级 | 待办数 | 说明 |
|--------|:------:|------|
| **P0 — 严重** | 2 | 线程安全 + 注册遗漏 |
| **P1 — 高** | 4 | 功能缺陷 & 测试缺口 |
| **P2 — 中** | 6 | API质量 & 小功能 |
| **P3 — 低** | 6 | 代码质量 & 文档 |
| **合计** | **18** | |

---

## P0 — 严重缺陷

### P0-1: Session 全局链表无锁保护
- **位置**: `src/middleware/session.c:24`
- **问题**: `static csilk_session_t* session_store = NULL` 全局链表在 `find_session`/`cleanup_expired`/`csilk_session_start/set/get/destroy` 中完全无锁。多 Worker 模式下数据竞争必崩。
- **对比**: `ratelimit.c` 和 `reflect.c` 已使用 `uv_mutex_t` 保护。
- **方案**: 用 `uv_mutex_t` + `uv_once_t` 包装所有 session 操作。
- **预估**: ~30 行

### P0-2: `test_async_keepalive.c` 未注册到 CMakeLists.txt
- **位置**: `CMakeLists.txt:186-221`（`add_csilk_test` 列表中无此文件）
- **问题**: 文件 `tests/test_async_keepalive.c` 已存在（包含 `is_async` keep-alive 回归测试），但不在 `add_csilk_test` 中，CI 从不执行。
- **影响**: `is_async` reset bug 可能静默回归。
- **方案**: 添加一行 `add_csilk_test(test_async_keepalive tests/test_async_keepalive.c)`
- **预估**: 1 行

---

## P1 — 高优先级

### P1-1: `write_chunk_frame` 超大数据静默丢弃
- **位置**: `src/core/context.c:671`
- **问题**: `if (total > UINT_MAX) return;` 导致 chunk 超过 4GB 时静默丢失。
- **方案**: 拆分为多次写入，或返回错误码让上层处理。
- **参照**: OPTIMIZE.md P6-3 已有记录但未修复。

### P1-2: CORS 中间件缺少 `Vary: Origin`
- **位置**: `src/middleware/cors.c:20-32`
- **问题**: 当 `Access-Control-Allow-Origin` 为具体域名（非 `*`）时，RFC 要求设置 `Vary: Origin`。当前从未设置。
- **方案**: 在设置 `Access-Control-Allow-Origin` 后追加 `Vary: Origin`。
- **预估**: ~3 行

### P1-3: `send_chunked_headers` 重复计算 strlen
- **位置**: `src/core/context.c:616`
- **问题**: `send_chunked_headers` 使用 `strlen(h->key/->value)`，但 `_csilk_send_response` (server.c:450-455) 已使用缓存的 `key_len/value_len`。chunked 路径未享受 P3-2 优化。
- **方案**: 改用 `h->key_len` / `h->value_len` 字段。
- **预估**: ~5 行

### P1-4: 无 OOM / malloc 失败测试
- **位置**: 全库 ~50+ 个 `malloc`/`calloc`/`realloc` 调用点无测试覆盖。
- **问题**: 大量函数的分配失败路径是 untested code，如 `map_set`、`csilk_get_cookie`、`csilk_arena_alloc` 等失败后静默返回 NULL，上层可能未检查。
- **方案**: 引入 `#ifdef TEST_OOM` hook 框架 + 关键路径覆盖。
- **预估**: ~150 行（含测试）

---

## P2 — 中优先级

### P2-1: `csilk_ctx_s` 内部结构暴露
- **位置**: `include/csilk_internal.h:45-69`
- **问题**: `csilk_ctx_s` 完整定义在公开 `include/` 下，`_internal_client` 等内部字段可见。
- **方案**: 将 `_csilk_send_response` 等内部接口移至内部头文件，遵循不透明类型模式。
- **注意**: 这是一个重构，需要同时更新 test_gzip.c 的 mock。
- **难度**: 高

### P2-2: `csilk_redirect` 可改进
- **位置**: `src/core/context.c:265-270`
- **问题**: (1) 固定分配 body "Redirecting..." 浪费 arena；(2) 未校验 status 是否为合法 3xx 码。
- **方案**: 改为不设置 body 直接 `csilk_status(c, status)`；添加状态码校验。

### P2-3: `csilk_ctx_cleanup` 未重置 `on_ws_message`
- **位置**: `src/core/context.c:165-208`
- **问题**: cleanup 会重置 `is_websocket`/`is_async` 等标志，但不会重置 `on_ws_message` 回调指针。keep-alive 复用时有风险。
- **方案**: cleanup 中设置 `c->on_ws_message = NULL`。

### P2-4: Email 验证不完整
- **位置**: `src/middleware/validate.c:19-27`
- **问题**: 仅检查 `@` 和 `.` 存在，不符合 RFC 5322。允许空格、非法字符等。

### P2-5: Session ID 使用 `rand()` 而非安全随机
- **位置**: `src/middleware/session.c:82-97`
- **问题**: `rand()` 非线程安全、可预测，且生成 32 字符 hex ID 的方式效率低（逐字符 `snprintf`）。

### P2-6: `csilk_set` storage 无上限
- **位置**: `src/core/context.c:278-311`
- **问题**: arena 分配的链表无长度限制，恶意 handler 可耗尽 arena。

---

## P3 — 低优先级 & 代码质量

### P3-1: Doxyfile `PROJECT_NUMBER` 未同步
- **位置**: `Doxyfile:3`
- **问题**: `PROJECT_NUMBER = 0.2.0`，实际为 0.2.1。

### P3-2: 重复 `typedef` `csilk_ctx_t`
- **位置**: `include/csilk.h:64,118`
- **问题**: `typedef struct csilk_ctx_s csilk_ctx_t;` 出现两次。

### P3-3: Request ID / 追踪中间件缺失
- 运维诊断不便，生产中常见需求。

### P3-4: Health check 端点缺失
- K8s 就绪/存活探针无标准端点。

### P3-5: `gzip.c` 未检查是否已压缩
- `Content-Encoding` 已有时不跳过；JPEG/PNG/MP4 照常压缩。

### P3-6: `test_config.c` 临时文件不清理
- `test_config_ext.yaml` 在测试崩溃后残留。

---

## 增量修复路线图

| 阶段 | 工作项 | 预估 | 说明 |
|------|--------|------|------|
| **Sprint 1** | P0-1 Session 加锁 | 0.5h | 简单加锁，高影响 |
| | P0-2 async_keepalive 注册 | 1min | 一行 CMake |
| | P1-2 Vary: Origin | 5min | 三行代码 |
| | P1-3 strlen→cached_len | 5min | 五行代码 |
| **Sprint 2** | P1-1 UINT_MAX 拆分写入 | 1h | 中等复杂度 |
| | P2-1 ctx 不透明化 | 2-3h | 重构，涉及 mock |
| | P2-2 redirect 改进 | 0.5h | 简单优化 |
| **Sprint 3** | P2-3 on_ws_message cleanup | 5min | 一行代码 |
| | P2-4 email 验证改进 | 1h | 实现 RFC 基本校验 |
| | P2-5 安全 session ID | 0.5h | /dev/urandom 读取 |
| **Sprint 4** | P1-4 OOM 测试 | 3-4h | 框架 + 关键路径覆盖 |
| | P3-1 ~ P3-6 | 2h | 小修小补 |
