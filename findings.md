# 研究与发现

## 核心架构洞察
- 框架基于 `libuv` (事件驱动) 和 `llhttp` (HTTP 解析)。
- 采用类似 Go Csilk 的洋葱模型中间件和前缀树(Radix Tree)路由。
- 当前代码库已包含 `tests/` 目录，需进一步确认现有测试覆盖率并补充边界用例。

## ASan 内存泄露排查发现
- **csilk_string**: 修复了局部栈分配导致的 stack-use-after-return。
- **libuv 句柄**: 修复了 timer handle 未关闭导致的 heap-use-after-free。
- **url_parser**: 修复了 client->current_url 和 request.path 的内存泄漏。

## WebSocket 实现调研
- WebSocket 需要接管 libuv 的 `uv_read_start`。当 HTTP 解析器发现 `Upgrade: websocket` 时，应停止 llhttp 解析，转而由 WebSocket 帧解析器处理后续数据。
- 需要增加 Base64 和 SHA1 的依赖用于握手协议。
