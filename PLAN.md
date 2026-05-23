# csilk 完善计划

## P0 - 严重缺陷 (已修复 ✓)

- [x] **修复响应头被忽略的 bug** — `csilk_set_header()` 设置的响应头在 `on_message_complete` 中从未被序列化到 HTTP 响应中，所有自定义响应头实际上被静默忽略
  - 修改: `src/server.c:on_message_complete()` — 遍历 `ctx.response.headers` 链表并写入响应
- [x] **修复 README 示例语法错误** — README 中使用 `^(csilk_ctx_t* ctx) { ... }` 的 block 语法在 C11 中非法，应改为标准函数指针语法
  - 修改: `README.md` — 所有示例改用标准 C 函数，匹配实际 API
- [x] **修复 llhttp ABI 不匹配** — 系统安装 llhttp 9.3 而 FetchContent 拉取 9.2，导致 `on_protocol` 字段偏移差异使 `on_url` 回调失效
  - 修改: `CMakeLists.txt` — 改用系统 llhttp 库，消除 ABI 冲突

## P1 - 核心功能增强

- [x] **优雅关闭** — 添加 `SIGINT`/`SIGTERM` 信号处理 + `csilk_server_stop()` 线程安全停止
  - 新增: `src/server.c` — `on_signal()`, `on_async_stop()` 回调, `csilk_server_stop()` 公共 API
  - 新增: `include/csilk.h` — `csilk_server_stop()` 声明
- [x] **超时与限制可配置** — 将硬编码的 idle timeout(5s)、body 大小限制(1MB)、listen backlog(128) 改为通过 `csilk_server_config_t` 结构体可配置
  - 新增: `include/csilk.h` — `csilk_server_config_t` 结构体, `csilk_server_set_config()` 声明
  - 修改: `src/server.c` — 所有硬编码常量替换为 `server->config.*`
- [x] **CORS 中间件** — 实现标准 CORS 中间件，支持可配置的 Origin/Methods/Headers/Credentials/Max-Age
  - 新增: `src/middleware/cors.c` — `csilk_cors_middleware()` 实现
  - 新增: `include/csilk.h` — `csilk_cors_config_t`, `csilk_cors_middleware()` 声明
- [x] **丰富 JSON API** — 添加 `csilk_bind_json_err()` (带错误反馈) 和 `csilk_json_error()` (JSON 错误响应)
  - 新增: `src/context.c` — `csilk_bind_json_err()`, `csilk_json_error()`
  - 新增: `include/csilk.h` — 对应声明

## P2 - 测试体系

- [ ] **集成测试框架** — 引入 CTest/CMocka/Criterion，替换现有的 raw assert 和手动打印
- [ ] **补全 keep-alive 测试** — `test_keepalive.c` 目前是空的 TODO 桩
- [ ] **补全集成/E2E 测试** — 目前的测试大多 mock 内部结构，仅有 `test_timeout` 做了真实的 server-client 交互
- [ ] **边界与负面测试** — 添加对畸形 URL、超长路径、header 注入、body 边界情况的测试
- [x] `run_tests` 目标不完整 — CMake 定义了 `run_tests` 但只编译 `test_main.c` 一个桩文件，没有集成 ctest

## P3 - 构建与工具链

- [ ] **CI/CD 配置** — 添加 GitHub Actions (.github/workflows/)，实现自动构建和测试
- [ ] **安装目标** — CMakeLists.txt 添加 `install()` 命令支持库和头文件的安装
- [ ] **静态库构建** — 支持 `BUILD_SHARED_LIBS` 选项，构建 `.a` 静态库
- [ ] **clang-tidy 集成** — 在构建中加入静态分析

## P4 - 安全加固

- [ ] **静态文件路径遍历防护增强** — 当前只拦截 `..`，应使用 `realpath()` 规范化路径后再校验前缀，防止编码绕过
- [ ] **请求头大小限制** — 添加对请求头的总大小限制，防止内存攻击
- [ ] **内存分配安全检查** — 检查所有 `malloc`/`realloc`/`strdup` 的返回值，失败时优雅返回错误而非崩溃

## P5 - 新功能 (可选)

- [ ] **Cookie 支持** — `csilk_get_cookie()` / `csilk_set_cookie()` API
- [ ] **文件上传** — multipart/form-data 解析
- [ ] **gzip 压缩** — 响应体压缩中间件
- [ ] **限流中间件** — 基于 IP 或路由的速率限制
- [ ] **WebSocket 支持**
- [ ] **HTTPS/TLS 支持**

## P6 - 文档完善

- [ ] **添加 CHANGELOG.md**
- [ ] **添加 CONTRIBUTING.md**
- [ ] **补充 Doxygen 注释** — 多个内部函数缺少文档
- [ ] **架构设计文档** — 说明 radix trie、handler chain、event loop 集成的设计原理
