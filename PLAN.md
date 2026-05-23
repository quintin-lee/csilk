# csilk 完善计划

## P0 - 严重缺陷

- [x] **修复响应头被忽略的 bug** — `csilk_set_header()` 设置的响应头在 `on_message_complete` 中从未被序列化到 HTTP 响应中
  - 修改: `src/server.c:on_message_complete()` — 遍历 `ctx.response.headers` 链表并写入响应
- [x] **修复 README 示例语法错误** — README 中使用 `^(csilk_ctx_t* ctx)` 的 block 语法在 C11 中非法
  - 修改: `README.md` — 所有示例改用标准 C 函数
- [x] **修复 llhttp ABI 不匹配** — 系统安装 llhttp 9.3 而 FetchContent 拉取 9.2
  - 修改: `CMakeLists.txt` — 改用系统 llhttp 库

## P1 - 核心功能增强

- [x] **优雅关闭** — `SIGINT`/`SIGTERM` 信号处理 + `csilk_server_stop()` 线程安全停止（`uv_async_send`）
- [x] **超时与限制可配置** — idle timeout、body 大小限制、header 大小限制、listen backlog 改为 `csilk_server_config_t` 可配置
- [x] **CORS 中间件** — 标准 CORS 中间件，Origin/Methods/Headers/Credentials/Max-Age 可配置
- [x] **丰富 JSON API** — `csilk_bind_json_err()` + `csilk_json_error()`
- [x] **IP 限流中间件** — 基于 IP 的 60s 滑动窗口速率限制，返回 `429 Too Many Requests`
  - 新增: `src/middleware/ratelimit.c` — 1024 slot 哈希表
- [x] **WebSocket 支持** — 握手、帧发送（含 length 编码）、帧解析（含 masking）
  - 新增: `src/websocket.c` — `csilk_ws_handshake()`, `csilk_ws_send()`, `csilk_ws_parse_frame()`
- [x] **Cookie API** — `csilk_get_cookie()` + `csilk_set_cookie()`（Max-Age/Path/Domain/Secure/HttpOnly）
  - 新增: `src/context.c` — cookie 解析与序列化
  - 新增: `include/csilk.h` — 对应声明
- [x] **`csilk_add_header()` 多值响应头** — 允许为同 key 添加多个响应头（用于 `Set-Cookie` 等）
  - 新增: `src/context.c` — 链表尾部追加实现

## P2 - 测试体系

- [x] **CTest 集成** — `enable_testing()` + `add_test()` 注册 **18 个测试**（17 个单元测试 + test_integration）
- [x] **集成/E2E 测试** — `test_integration.c` (365 行)、`test_timeout.c`、`test_keepalive.c` (144 行) 真实 server-client 交互
- [x] **keep-alive 测试** — `test_keepalive.c` 完整的两请求时序验证（`Connection: keep-alive` → `close`）
- [x] **边界与负面测试** — `test_edge.c` (159 行) 覆盖畸形 URL、超长路径、header 注入、body 边界
- [x] **各模块单元测试** — logger/static/context/router/radix/group/headers/server/query/json/recovery/auth/ws/config/cookie 共 17 个单元测试
- [x] **Fuzz 测试** — CI 集成 libFuzzer（`fuzz_test`, `-max_total_time=10`）
- [x] **`run_tests` 快捷目标** — `make run_tests` 用于一键运行所有测试

## P3 - 构建与工具链

- [x] **CI/CD 配置** — `build.yml`（Debug/Release 矩阵 + clang-tidy + 单元/集成测试）+ `ci.yml`（ASAN + fuzz）
- [x] **安装目标** — `install(TARGETS ... EXPORT csilk-targets ...)` + 头文件安装 + CMake config 导出
- [x] **clang-tidy 集成** — `make tidy` target，`clang-analyzer-*` + `performance-*` 检查
- [x] **静态/动态库支持** — 默认静态；`-DBUILD_SHARED_LIBS=ON` 构建动态库
- [x] **CHANGELOG.md** — 记录版本历史
- [x] **CONTRIBUTING.md** — 贡献指南

## P4 - 安全加固

- [x] **静态文件路径遍历防护** — 使用 `realpath()` 规范化 root_dir 和文件路径，校验 resolved_file 前缀是否匹配 resolved_root（不再仅靠 `strstr("..")`）
  - 修改: `src/middleware/static.c` — `csilk_static()` 完整重写路径校验
- [x] **请求头大小限制** — 添加 `max_header_size` 配置项（默认 64KB），在 `on_header_field`/`on_header_value` 中累加检查
  - 修改: `src/server.c` — `on_message_begin` 重置计数器，超限返回 `HPE_USER`
- [x] **内存分配安全检查** — 检查所有 `malloc`/`realloc`/`strdup` 返回值，失败时优雅返回
  - 修改: `src/config.c` — 所有 strdup 加 NULL 检查 + `error` 传播
  - 修改: `src/middleware/static.c` — malloc 返回值检查，失败返回 500
  - 修改: `src/context.c` — `csilk_string()` 修复空 body 处理

## P5 - 新功能 (可选)

- [x] **反射与 JSON 自动化绑定** — `CSILK_REGISTER_REFLECT` 与 `csilk_bind_reflect` API
- [ ] **文件上传** — multipart/form-data 解析
- [ ] **gzip 压缩** — 响应体压缩中间件
- [ ] **HTTPS/TLS 支持**

## P6 — 已归档（全部完成）

以下分类已全部完成，保留作为索引：

- **Doxygen 注释** — 所有头文件 + .c 文件全覆盖，零警告
- **README 更新** — Documentation 章节、Features、项目结构
- **CHANGELOG.md** — `CHANGELOG.md` (33 行)
- **CONTRIBUTING.md** — `CONTRIBUTING.md` (50 行)
