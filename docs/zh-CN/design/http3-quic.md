# HTTP/3 & QUIC 可行性评估

> **日期**: 2026-05-30 | **版本**: 0.3.0

## 概述

HTTP/3（QUIC）集成**可行但工作量大**。主要障碍是缺乏一个稳定、维护良好且许可证宽松的 QUIC 库，同时能够干净地集成到 libuv 的事件循环中。

## QUIC 库选项

| 库 | 语言 | 许可证 | 状态 | 集成难度 |
|---|:----:|:------:|:----:|:--------:|
| **lsquic** | C | MIT | 活跃（LiteSpeed） | 中等 — 事件引擎抽象 |
| **msquic** | C | MIT | 活跃（Microsoft） | 高 — 以 Windows 为先，POSIX 支持不成熟 |
| **ngtcp2** | C | MIT | 活跃（nghttp2 组织） | 中等 — 设计上配合 nghttp3 |
| **quiche** | C/Rust | BSD-2 | 活跃（Cloudflare） | 中等 — Rust 核心，C FFI |
| **quictls** | C | Apache-2 | 活跃（OpenSSL 分支） | 低 — 仅 TLS，需要传输层 |

## 推荐方案: ngtcp2 + nghttp3

**理由**: csilk 已在使用 nghttp2 处理 HTTP/2。ngtcp2 库（QUIC 传输层）和 nghttp3（HTTP/3 映射）来自同一组织，共享编码规范。两者均为 MIT 许可。

**架构契合度**: ngtcp2 提供回调驱动的 API，可适配到 libuv 的事件循环，与 nghttp2 的集成方式类似。

## 集成计划（v1.0+）

### 步骤 1: QUIC 传输层（ngtcp2）

- 将 `ngtcp2` 和 `ngtcp2_crypto_quictls` 添加为 FetchContent 依赖
- 创建连接到 libuv UDP 套接字的 `src/protocols/quic.c`
- 处理 QUIC 握手、连接迁移、0-RTT
- 预估约 2,000 行代码

### 步骤 2: HTTP/3 映射（nghttp3）

- 将 `nghttp3` 添加为 FetchContent 依赖
- 在 QUIC 传输层之上构建 HTTP/3
- 映射 QPACK（头部压缩）和流类型
- 预估约 1,500 行代码

### 步骤 3: ALPN 与协议选择

- 扩展 `tls.c` ALPN 回调，在 `h2` 和 `http/1.1` 之外同时提供 `h3`
- 添加 `Alt-Svc` 头部支持，用于 HTTP/3 升级提示
- 预估约 300 行代码

### 步骤 4: 统一分发

- 与 HTTP/1.1 和 HTTP/2 共享相同的路由、中间件和处理器链
- 协议对处理器透明 — 现有处理器无需修改即可工作

## 预估工作量

| 阶段 | 工作量 | 风险 |
|-----|:------:|:----:|
| QUIC 传输层 | 3-4 周 | 中等 — UDP 事件循环集成 |
| HTTP/3 映射 | 2-3 周 | 低 — nghttp3 API 清晰 |
| ALPN 与发现 | 1 周 | 低 |
| 测试与加固 | 2-3 周 | 中等 |
| **总计** | **8-11 周** | |

## 需关注的依赖

- ngtcp2 需要支持 QUIC 的 OpenSSL（OpenSSL 3.2+ 或 quictls 分支）
- QPACK（nghttp3）增加约 200KB 二进制大小
- QUIC 使用 UDP — 可能无法通过所有防火墙/负载均衡器

## 结论

**推迟到 v1.1+**。HTTP/3 仅占全球 Web 流量的不到 30%（2026 年）。csilk 当前的 HTTP/2 支持覆盖了绝大多数用例。当 ngtcp2/nghttp3 达到 v1.0 且 OpenSSL QUIC 支持稳定时重新评估。

**v1.1+ 要求**: QUIC 传输层 **必须** 使用 ngtcp2（与 nghttp2 同一组织）。HTTP/3 映射 **必须** 使用 nghttp3。OpenSSL **必须** 为 3.2+ 或 quictls 分支以支持 QUIC 加密。二进制大小增加 **不应** 超过约 200KB（QPACK）。UDP 事件循环集成 **不得** 阻塞主 libuv 线程。
