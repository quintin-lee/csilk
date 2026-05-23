# 研究与发现

## 核心架构洞察
- 框架基于 `libuv` (事件驱动) 和 `llhttp` (HTTP 解析)。
- 采用类似 Go Gin 的洋葱模型中间件和前缀树(Radix Tree)路由。
- 当前代码库已包含 `tests/` 目录，需进一步确认现有测试覆盖率并补充边界用例。

## ASan 内存泄露排查发现
- **gin_string**: 修复了局部栈分配导致的 stack-use-after-return。
- **libuv 句柄**: 修复了 timer handle 未关闭导致的 heap-use-after-free。
- **url_parser**: 修复了 client->current_url 和 request.path 的内存泄漏。
