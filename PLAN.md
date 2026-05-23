# csilk 完善计划

> 最后更新: 2026-05-23 | 基于 `7fc37cc`

## P0 — 严重缺陷（数据竞争与安全）

- [x] **限流中间件数据竞争** — `src/middleware/ratelimit.c` 的静态哈希表（`ip_table`、`ip_count`）无任何锁保护，多线程下数据竞争
- [x] **`csilk_get_client_ip` 静态缓冲区竞态** — `src/server.c:461` 使用 `static char ip[46]` 共享缓冲区，并发请求会互相覆盖
- [x] **`csilk_ctx_cleanup` 未释放 `request.path`** — `src/context.c:167-197` 遍历清理 response headers / body / params 但遗漏 `request.path`
- [x] **Config 中 `strdup` 的字符串永不释放** — `src/config.c:78-109` 中 `logger.file_path`、`cors.allow_origin*`、`static_files.*` 无对应清理函数
- [x] **`advanced_server.c` 编译失败** — `examples/advanced_server.c:72` 调用 `csilk_log_init(CSILK_LOG_DEBUG, NULL)` 但当前 API 为 `csilk_log_init(csilk_log_config_t)`

## P1 — 内存安全与资源泄漏

- [x] **`csilk_server_free` 未释放 libuv 资源** — `src/server.c:498-501` 仅 `free(server)`，未 `uv_close` 各 handle（server_handle / sig_handle / async_handle）
- [x] **`csilk_json` 重复调用泄漏前 body** — `src/context.c:318-329` 未检测 `response.body` 是否已分配就直接覆盖
- [x] **`csilk_set_cookie` snprintf 溢出风险** — `src/context.c:288-313` 固定 `char buf[1024]`，长 value 静默截断
- [x] **`on_url` / `on_header_field` unchecked malloc** — `src/server.c:139,168` 未检查返回值
- [x] **`get_next_segment` unchecked malloc** — `src/router.c:86` 未检查返回
- [x] **`on_header_value` realloc 失败泄漏旧指针** — `src/server.c:184` realloc 失败时 `current_header_value` 仍指向旧内存但被赋值 NULL

## P2 — 中间件系统重构

- [x] **参数化中间件可以通过 Context Storage 支持** — 新增 `csilk_set()` / `csilk_get()` 允许中间件间共享配置
- [x] **新增全局（server-level）中间件** — `csilk_server_use(csilk_server_t*, csilk_handler_t)` 自动应用到全部路由
- [x] **新增逐路由中间件** — `csilk_group_add_handlers()` 支持在具体路由上挂载中间件链
- [x] **CSRF token 校验精确化** — `src/middleware/csrf.c:34` 改用 `csilk_get_cookie` 进行精确匹配

## P3 — 配置系统完善

- [x] **YAML 已解析 `max_header_size`**
- [ ] **YAML 不支持中间件配置**
- [ ] **配置缺少语义校验**
- [x] **`csilk_config_free()` 已添加到公共头文件**
- [x] **example_server 已从配置清理资源**

## P4 — 代码质量与技术债

- [x] **`_gin_log_internal` 已重命名为 `_csilk_log_internal`**
- [x] **版本号已统一为 0.2.0**
- [x] **Server 使用 Logger 而非 printf**
- [x] **Arena chunk size 已使用常量 `CSILK_DEFAULT_ARENA_SIZE`**
- [x] **SHA1 `utils.c` 死代码已清理**
- [x] **`strncpy` 在 ratelimit.c 已保证 null 终止**
- [x] **Doxyfile 已强化**
- [ ] **`docs/html/` `docs/latex/` 预生成产物不应在版本控制中**

## P5 — 测试覆盖扩展

- [ ] **`test_static.c` 无有效断言** — 两测试仅打印 "passed (assumed)"，未验证响应状态码或 body 内容
- [ ] **`csilk_get_client_ip` 无测试** — 未在任何测试中覆盖
- [ ] **`csilk_add_header` 无测试** — 新增的多值响应头 API 未覆盖
- [ ] **`csilk_bind_json_err` / `csilk_json_error` 无测试**
- [ ] **`csilk_arena_*` 无独立测试**
- [ ] **`csilk_ws_send` 无测试** — `test_ws.c` 只测了 parse_frame
- [ ] **`test_ws.c` 覆盖不足** — 缺少: unmasked frames, FIN=0 分片帧, Ping/Pong/Close 控制帧, 大 payload (>125B, >65535B), 实际 masking, 畸形帧
- [ ] **`test_cookie.c` 覆盖不足** — 缺少: cookie 删除 (Max-Age=0), 长 value 截断, 畸形 Cookie header, 多个 cookie 获取
- [ ] **集成测试中 `|| true` 吞失败** — `build.yml` 中 `ctest ... -E test_integration || true` 静默忽略集成测试失败

## P6 — 文档与提效

- [ ] **README 未列出 `libyaml` 依赖** — CMake `find_package(yaml)` 但 README Dependencies 节未包含
- [ ] **README 未列出新功能** — Cookies、CORS、CSRF、RateLimit、YAML Config、Reflection 均无说明
- [ ] **README 构建命令过时** — 仍展示 `./tests/test_logger` 等路径命令，应改为 `make run_tests` 或 `ctest`
- [ ] **`docs/ARCH.md` 使用旧项目名** — 标题 "Server-C" 应统一为 "csilk"

## P7 — 新功能探索

- [ ] **文件上传** — multipart/form-data 解析
- [ ] **gzip 压缩** — 响应体压缩中间件
- [ ] **HTTPS/TLS 支持**
- [ ] **Server-Sent Events (SSE)**

---

## 状态一览

| 优先级 | 待办 | 说明 |
|--------|:----:|------|
| P0 | 5 | 数据竞争 + 泄漏 + 编译错误 |
| P1 | 6 | 内存安全与资源管理 |
| P2 | 4 | 中间件系统设计缺陷 |
| P3 | 5 | 配置系统不完整 |
| P4 | 8 | 技术债与代码质量 |
| P5 | 9 | 测试覆盖缺失 |
| P6 | 4 | 文档待更新 |
| P7 | 4 | 探索性新功能 |
| **合计** | **45** | |
