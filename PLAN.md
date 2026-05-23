# csilk 完善计划

## P0 - 严重缺陷

- [x] **修复响应头被忽略的 bug** — `csilk_set_header()` 设置的响应头在 `on_message_complete` 中从未被序列化到 HTTP 响应中
  - 修改: `src/server.c:on_message_complete()` — 遍历 `ctx.response.headers` 链表并写入响应
- [x] **修复 README 示例语法错误** — README 中使用 `^(csilk_ctx_t* ctx) { ... }` 的 block 语法在 C11 中非法
  - 修改: `README.md` — 所有示例改用标准 C 函数
- [x] **修复 llhttp ABI 不匹配** — 系统安装 llhttp 9.3 而 FetchContent 拉取 9.2，导致 `on_protocol` 字段偏移差异
  - 修改: `CMakeLists.txt` — 改用系统 llhttp 库

## P1 - 核心功能增强

- [x] **优雅关闭** — 添加 `SIGINT`/`SIGTERM` 信号处理 + `csilk_server_stop()` 线程安全停止
- [x] **超时与限制可配置** — idle timeout、body 大小限制、listen backlog 改为 `csilk_server_config_t` 可配置
- [x] **CORS 中间件** — 标准 CORS 中间件，支持可配置的 Origin/Methods/Headers/Credentials/Max-Age
- [x] **丰富 JSON API** — 添加 `csilk_bind_json_err()` 和 `csilk_json_error()`
- [x] **IP 限流中间件** — 基于 IP 的滑动窗口速率限制，返回 `429 Too Many Requests`
  - 新增: `src/middleware/ratelimit.c` — 1024 slot 哈希表, 60s 窗口
- [x] **WebSocket 支持** — 完整 WebSocket 握手、帧发送、帧解析（含 masking）
  - 新增: `src/websocket.c` — `csilk_ws_handshake()`, `csilk_ws_send()`, `csilk_ws_parse_frame()`

## P2 - 测试体系

- [x] **CTest 集成** — `enable_testing()` + `add_test()` 注册 16 个测试可执行文件
- [x] **集成/E2E 测试** — `test_integration.c` (365 行), `test_timeout.c`, `test_keepalive.c` (144 行) 均有真实 server-client 交互
- [x] **keep-alive 测试** — `test_keepalive.c` 已实现完整的两请求时序测试
- [x] **边界与负面测试** — `test_edge.c` (159 行) 覆盖畸形 URL、超长路径、header 注入、body 边界
- [x] **各模块单元测试** — 路由、分组、JSON、配置、查询参数、WebSocket、静态文件、recovery、auth、logger、server、headers 均有对应测试文件
- [x] **Fuzz 测试** — CI 集成 libFuzzer (`fuzz_test`, `-max_total_time=10`)
- [x] **`run_tests` 快捷目标** — CMakeLists.txt 缺少 `make run_tests` 自定义 target（当前通过 `ctest` 直接运行）

## P3 - 构建与工具链

- [x] **CI/CD 配置** — `.github/workflows/build.yml`（矩阵: Debug/Release, clang-tidy, 单元+集成测试）+ `.github/workflows/ci.yml`（ASAN, fuzz 测试）
- [x] **安装目标** — CMakeLists.txt 完整 `install()` 命令（库、头文件、CMake config 导出）
- [x] **clang-tidy 集成** — 自定义 `tidy` target，启用 `clang-analyzer-*` + `performance-*` 检查
- [x] **静态库构建显式支持** — 目前靠 CMake 默认行为工作，缺少文档说明 `-DBUILD_SHARED_LIBS=ON`

## P4 - 安全加固

- [x] **静态文件路径遍历防护增强** — 当前只拦截 `..`，应使用 `realpath()` 规范化路径后再校验前缀，防止编码绕过
- [x] **请求头大小限制** — 添加对请求头的总大小限制，防止内存攻击
- [x] **内存分配安全检查** — 检查所有 `malloc`/`realloc`/`strdup` 的返回值

## P5 - 新功能 (可选)

- [x] **Cookie 支持** — `csilk_get_cookie()` / `csilk_set_cookie()` 专用 API
- [ ] **文件上传** — multipart/form-data 解析
- [ ] **gzip 压缩** — 响应体压缩中间件
- [ ] **HTTPS/TLS 支持**

## P6 - 文档完善

- [x] **Doxygen 注释** — 所有头文件公共 API 完整文档（`@brief`、`@param`、`@return`）；所有 `.c` 文件均有 `@file` 块；所有静态/内部函数均有文档；Doxygen 零警告
- [x] **README 更新** — 添加 Documentation 章节（覆盖率表格）、更新 Features、项目结构
- [x] **架构设计文档** — 更新到 PLAN.md 中
- [x] **添加 CHANGELOG.md**
- [x] **添加 CONTRIBUTING.md**
