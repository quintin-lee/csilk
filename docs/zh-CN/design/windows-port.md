# Windows（MSVC）支持评估

> **日期**: 2026-05-30 | **状态**: 预研究
>
> **Windows 约束**: MSVC **不得** 用于原生构建 — 不支持 C23 特性（`static constexpr`、`nullptr`）。通过 MinGW-w64 进行交叉编译 **应该** 是主要的 Windows 路径。基于 libuv 的 I/O **必须** 工作（IOCP 后端完全支持）。仅 POSIX 的代码路径（`pthread`、`sys/socket.h`、`unistd.h`）**应该** 通过 `#ifdef` 防护隔离。

## 概述

由于深度 POSIX API 依赖，通过 MSVC 支持 Windows **短期内不可行**。通过 MinGW 或 WSL2 进行交叉编译更实用。

## API 兼容性矩阵

| API/依赖 | Windows 支持 | 说明 |
|---------|:-----------:|------|
| **libuv** | 完整 | 一流的 Windows 支持（IOCP 后端） |
| **llhttp** | 完整 | 纯 C，无 POSIX 依赖 |
| **nghttp2** | 完整 | 通过 CMake 支持 Windows 构建 |
| **cJSON** | 完整 | 纯 C，跨平台 |
| **pthread** | 通过 pthreads-win32 | API 面不同 |
| **sys/socket.h** | Winsock2 | 不同的头文件、类型、常量 |
| **unistd.h** | io.h / process.h | 不同的头文件映射 |
| **sendfile()** | TransmitFile() | 完全不同的 API |
| **setjmp/longjmp** | 部分 | 可用但语义不同 |
| **signal()** | 有限 | 无 SIGPIPE，不同的信号模型 |
| **fork()** | 无 | Windows 使用 CreateProcess |
| **SO_REUSEPORT** | 无 | Windows 对 SO_REUSEADDR 的处理不同 |
| **mmap()** | VirtualAlloc/MapViewOfFile | 完全不同的 API |
| **OpenSSL** | 完整 | 提供 Windows 二进制文件 |
| **zlib** | 完整 | 支持 Windows |
| **libcurl** | 完整 | 支持 Windows |

## 受影响的源文件

| 文件 | 问题 |
|------|------|
| `src/core/server.c` | `SO_REUSEPORT`、`pthread_barrier_t`、`fork()`、信号处理 |
| `src/core/connection.c` | `accept()`、套接字选项、`sendfile()` |
| `src/core/tls.c` | BIO 对、ALPN（通过 OpenSSL 工作） |
| `src/core/http1.c` | `writev()` → `WSASend()` |
| `src/core/logger.c` | ANSI 颜色转义码（Windows 终端） |
| `src/core/config.c` | 文件路径处理、`realpath()` |
| `src/middleware/static.c` | `sendfile()` → `TransmitFile()` |
| `src/middleware/gzip.c` | zlib 正常工作，线程池通过 libuv |

## 实际替代方案

### 方案 A: MinGW（Windows 上的 GCC）

- 以最小的更改编译 POSIX 代码
- libuv、OpenSSL、zlib 全部正常工作
- 通过 winpthreads 提供 pthreads
- 工作量：构建系统约 1 周，修复约 2 周

### 方案 B: WSL2（Windows 上的 Linux）

- 零代码更改 — 作为原生 Linux 二进制运行
- 开发最佳选择
- 生产部署：通过 Docker 在 Windows 上容器化

### 方案 C: 完整 MSVC 移植

- 需要在代码库中通过 `#ifdef _WIN32` 防护
- 将所有 POSIX API 替换为 Windows 等价物
- 预估工作量：8-12 周
- 持续的维护负担

## 结论

**不建议用于 v1.0**。POSIX 依赖面过大，不适合实用的 MSVC 移植。Windows 用户应使用 WSL2 进行开发，或在 Docker 中运行 csilk。如果需要原生 Windows 二进制文件，MinGW 交叉编译是最可行的路径 — 在 v1.0 后评估。
