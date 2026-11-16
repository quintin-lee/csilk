# Windows 支持评估 — v1.2.0

## 结论：不实现 Windows 支持

### 理由

1. **项目定位**：csilk 是 Linux-first 高性能 HTTP 框架，目标场景为云原生 / 容器化部署。Windows 不是目标平台。

2. **技术障碍**（不可克服）：
   - **io_uring**：Linux 内核接口，Windows 无等价物
   - **AF_XDP**：Linux kernel bypass，Windows 用 NDIS/WFP（完全不同的 API）
   - **pthread_barrier_t**：Windows 无内置 barrier，需 shim
   - **libuv**：Windows 支持有限（无 io_uring 后端）
   - **epoll/kqueue**：Windows 用 IOCP，需要完全不同的 event loop 抽象

3. **维护成本**：
   - CI 矩阵翻倍（Windows + Linux + macOS × Debug + Release × ASAN/TSAN）
   - 依赖矩阵复杂（OpenSSL-Win, libuv-Win, nghttp2-Win, llhttp-Win, yyjson-Win）
   - 测试适配（Win32 线程、文件路径、信号处理）

4. **用户需求**：目前无 Windows 用户反馈或 issue。

### 保留的 CMake 防护

```cmake
if(NOT APPLE AND NOT WIN32)
    # Linux-only targets (io_uring, AF_XDP)
endif()
```

### 未来可能性

若 v2.0+ 社区有强需求：
- 需要实现 `csilk_io_*` 的 IOCP 后端（~4 周）
- `csilk_thread_*` 的 Win32 线程 shim（~2 周）
- 测试适配（~2 周）
- **总计**：~8 周，需 2 名开发者

## 建议

在 README 中明确标注 "Windows 不在支持范围内"。
