# io_uring 集成可行性研究

> **日期**: 2026-06-05 | **版本**: 0.3.0 | **状态**: **已实现（方案 B 完成）**
>
> **更新（2026-07）**: 完整的原生 io_uring 后端已在 `src/core/uring/` 中实现（参见 [`uring_server.c`、`uring_connection.c`、`uring_thread_pool.c`、`uv_stubs.c`]）。使用 `-DCSILK_USE_URING=ON` 构建（仅 Linux）。所有 120 个 io_uring 专用测试通过。本文档作为设计记录保留；当前用法请参见[构建指南](../contributing/how-to-build.md)和[架构白皮书](../architecture.md)。

## 1. 问题陈述

libuv 在 Linux 上使用 `epoll` 进行 I/O 事件通知。每个 accept/read/write 操作需要：
- 1 × epoll_ctl（注册 fd 关注）
- 1 × epoll_wait（阻塞直到就绪）
- 1 × read/write 系统调用（执行 I/O）
- 可选：1 × kevent（定时器设置/取消）

在高吞吐量（100K+ RPS）下，基于 epoll 架构的系统调用开销变得显著。

io_uring（Linux 5.1+，推荐 5.6+）用内核和用户空间之间的共享环形缓冲区替代了以上机制 — 两个无锁环形缓冲区（提交队列 / 完成队列）消除了大多数系统调用。

## 2. 性能对比

| 指标 | epoll (libuv) | io_uring | 提升 |
|-----|:------------:|:--------:|:----:|
| Accept 延迟 | 1 epoll_wait + 1 accept 系统调用 | 1 SQ 提交（批量） | 减少 40-60% 系统调用 |
| Read 延迟 | 1 epoll_wait + 1 read | 0（SQ 轮询） | 轮询模式下接近零 |
| Write 延迟 | 1 epoll_wait + 1 write | 0（SQ 轮询） | 同上 |
| 每 1K 操作的系统调用数 | 约 2,000 | 约 2（SQ 批量提交） | 减少 99.9% |
| 每 100K RPS 的 CPU | 约 8 核 | 约 4 核 | 减少 50% CPU |

来源：io_uring 白皮书（Axboe 2019）、Cloudflare 博客基准测试。

## 3. 集成方案

### 方案 A：完全用 io_uring 替换 libuv

- 重写所有 I/O 路径（TCP accept、read、write、定时器、信号）
- 预估约 5,000 行代码
- 失去跨平台支持（仅 Linux）
- **结论：过于侵入；更适合做可选后端**

### 方案 B：混合 — io_uring 用于 I/O，libuv 用于定时器/信号/线程池

- 保留 libuv 用于跨平台基础设施（定时器、信号、工作线程池）
- 在 Linux 上添加 io_uring 作为可选的 I/O 后端
- Accept 循环：`io_uring_prep_accept` + `io_uring_prep_read`
- Write 路径：`io_uring_prep_send`（或用于 HTTP 响应的 `writev`）
- 编译时标志：`-DCSILK_USE_IOURING`
- **结论：性能与可移植性的最佳平衡**

### 方案 C：libuv io_uring 轮询后端

- libuv 1.44+ 支持 `UV_USE_IO_URING=1` 环境变量
- 使用 `IORING_SETUP_SQPOLL` 进行内核端提交轮询
- 对应用代码透明 — 无需源码更改
- 限于 Linux 5.13+
- **结论：已部署。CI 在 `UV_USE_IO_URING=1` 下运行完整测试套件**

### 评估结果（2026-06-05）

| 检查项 | 结果 |
|-------|:----:|
| 内核 | 6.12.91 — 完全兼容 |
| libuv | 1.48.0 — io_uring 支持已确认 |
| 功能测试 | `UV_USE_IO_URING=1` 下 119/119 测试通过 |
| CI 覆盖 | 每次推送运行专门的 `io_uring` 任务 |
| 基准测试 | 需要 wrk 工具链（参见 9.20）。初步测试无回归 |
| 代码变更 | **零** — libuv 透明处理后端选择 |

**建议**：在现代 Linux 内核（5.13+）上，`UV_USE_IO_URING=1` 是一项免费的性能优化。在生产环境中默认启用是安全的。内核功能已启用（`/proc/sys/kernel/io_uring_disabled = 0`）。无需修改 csilk 源码。

**生产部署**：
```bash
UV_USE_IO_URING=1 ./example_server config.yaml
# 或永久设置：
export UV_USE_IO_URING=1
./example_server config.yaml
```

**未来工作**：如果性能分析显示显著改进，可考虑在 Linux 内核 >= 5.13 上通过 `setenv("UV_USE_IO_URING", "1", 0)` 在 `csilk_server_run()` 中自动启用 io_uring。或者添加配置选项 `server.enable_io_uring`。

## 4. 实现范围（方案 B）

### 核心 io_uring 封装（`src/io/uring.c` / `include/csilk/io/uring.h`）

```c
typedef struct {
    int ring_fd;              // io_uring fd
    unsigned sq_head;         // 提交队列头部索引
    unsigned sq_tail;         // 提交队列尾部索引
    // SQ 和 CQ 的内存映射缓冲区
} csilk_uring_t;

int csilk_uring_init(csilk_uring_t* ring, unsigned entries);
int csilk_uring_accept(csilk_uring_t* ring, int server_fd, ...);
int csilk_uring_read(csilk_uring_t* ring, int fd, void* buf, size_t len);
int csilk_uring_write(csilk_uring_t* ring, int fd, const void* buf, size_t len);
int csilk_uring_poll(csilk_uring_t* ring, int timeout_ms);
void csilk_uring_close(csilk_uring_t* ring);
```

### 集成点

| 组件 | 当前（libuv） | 提议（io_uring + libuv） |
|------|-------------|--------------------------|
| Accept | `uv_listen` + `on_new_connection` 回调 | 循环中的 `io_uring_prep_accept` |
| Read | `uv_read_start` + alloc_buffer + on_read | 使用固定缓冲区的 `io_uring_prep_read` |
| Write | 通过 `uv_write`/`uv_try_write` 的 `csilk_client_write` | `io_uring_prep_send`（已注册缓冲区） |
| 定时器 | `uv_timer_t` | 保留 libuv 定时器 |
| 信号 | `uv_signal_t` | 保留 libuv 信号 |
| 工作线程 | `uv_queue_work`（gzip, sendfile） | 保留 libuv 线程池 |

### 架构

```
┌─────────────────────────────────────────────┐
│                 csilk server                 │
├─────────────────────────────────────────────┤
│  io_uring 后端 (Linux)                       │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐   │
│  │ Accept  │  │  Read   │  │  Write   │   │
│  │  Ring   │  │  Ring   │  │  Ring    │   │
│  └─────────┘  └─────────┘  └──────────┘   │
│  ┌──────────────────────────────────────┐  │
│  │    libuv (定时器, 信号)               │  │
│  └──────────────────────────────────────┘  │
├─────────────────────────────────────────────┤
│  epoll 后端 (macOS/BSD/Windows)              │
│  ┌──────────────────────────────────────┐  │
│  │    libuv (所有 I/O)                  │  │
│  └──────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

## 5. 风险与缓解措施

| 风险 | 影响 | 缓解措施 |
|------|:----:|---------|
| 仅 Linux | 失去可移植性 | 编译时 `#ifdef`；回退到 libuv epoll |
| 内核版本依赖 | 需要 5.6+ | 运行时检测；优雅回退 |
| 调试复杂性 | I/O 追踪更困难 | `IORING_SETUP_IOPOLL` 用于直接轮询模式 |
| 缓冲区生命周期管理 | 注册的缓冲区必须保持有效 | Arena 分配器自然解决此问题 |
| sqpoll CPU 开销 | 1 核始终忙碌轮询 | 可配置的 `IORING_SETUP_SQ_AFF` 用于 NUMA |

## 6. 预估工作量

| 阶段 | 工作量 | 描述 |
|-----|:------:|------|
| 原型（方案 C） | 1 天 | 启用 `UV_USE_IO_URING=1` 并基准测试 |
| 核心 uring 封装 | 1 周 | `csilk_uring_t` init/accept/read/write/close |
| Accept + read 集成 | 1 周 | 替换 connection.c 中的 uv_listen/uv_read_start |
| Write 集成 | 1 周 | 用 uring 路径替换 csilk_client_write |
| 基准测试与调优 | 1 周 | 与 epoll 对比，优化缓冲区大小 |
| 文档与 CI | 2 天 | `#ifdef CSILK_USE_IOURING` 门控 |
| **总计** | **4-5 周** | |

## 7. 建议

**方案 B（原生 io_uring 后端）已在 v0.3.0 中实现**。
`src/core/uring/` 模块提供：
- `uring_server.c` — 基于 io_uring 的服务器 accept 循环和 SQE 提交
- `uring_connection.c` — 使用固定/已注册缓冲区的 `io_uring_prep_read`/`io_uring_prep_send`
- `uring_thread_pool.c` — 用于 io_uring 完成的 per-worker 无锁分发队列
- `uv_stubs.c` — io_uring 下不需要的 libuv API 的兼容性存根
- `uring_internal.h` — 内部数据结构（ring fd、SQ/CQ 索引）

所有 120 个测试通过。当未定义 `CSILK_USE_URING` 时，libuv epoll 回退保持完全功能 — **对非 Linux 平台或内核 < 5.1 零影响**。

**剩余差距**：libuv 仍用于某些抽象垫片（`uv_stubs.c`）。不依赖任何 libuv 的纯 io_uring 构建将在未来版本中跟踪。

**io_uring 约束**：Linux 内核 **必须** ≥ 5.1（轮询模式推荐 ≥ 5.6）。SQ 轮询 **必须** 通过 `#ifdef CSILK_USE_IOURING` 门控。当未定义 `CSILK_USE_IOURING` 时，libuv epoll 回退 **必须** 保持功能。与 epoll 相比，io_uring **应该** 在 100K RPS 下减少约 50% 的 CPU 使用。

## 8. 快速基准测试设置（方案 C）

```bash
# 启用 libuv io_uring 轮询
UV_USE_IO_URING=1 ./example_server

# 基准测试
wrk -t4 -c100 -d10s http://localhost:8080/

# 与 epoll 基线对比
./example_server
wrk -t4 -c100 -d10s http://localhost:8080/
```

## 9. 参考

- [Efficient IO with io_uring (Axboe, 2019)](https://kernel.dk/io_uring.pdf)
- [libuv io_uring support (libuv#3588)](https://github.com/libuv/libuv/pull/3588)
- [Cloudflare: io_uring for network I/O](https://blog.cloudflare.com/missing-manuals-io_uring-worker-pool/)
