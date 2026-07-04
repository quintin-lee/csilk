# macOS 平台支持评估

> **日期**: 2026-05-31 | **状态**: **已支持**（macOS 14 ARM64 CI 自 2026-05-31 起活跃）
>
> **macOS 约束**: CI **必须** 使用 Homebrew LLVM 19+（`brew install llvm`）— Apple Clang 不支持 C23 `constexpr`。`pthread_barrier_t` 不可用 — 多 worker 模式 **应该** 使用基于 POSIX 信号量的回退。所有基于 libuv 的 I/O 路径 **必须** 在 macOS 上工作一致（kqueue 后端）。

## 背景

macOS CI（macos-14 ARM64）现在是 CI 矩阵的一部分。以下问题已按以下方式解决：

## 已解决问题

### 1. 编译器：Apple Clang 缺少 C23

| 特性 | 状态 |
|-----|:----:|
| `static constexpr` | 已解决 — CI 中使用 Homebrew LLVM 19+（`brew install llvm`） |
| `nullptr` 关键字 | 已解决 — 通过 LLVM 19+ |
| `bool` 关键字 | 已解决 — 通过 LLVM 19+ |

### 2. cJSON C23 关键字冲突

**修复**: 在 CMAKE_C_FLAGS 中添加 `-Wno-keyword-macro`。所有平台通用。

### 3. Apple SDK 弃用警告

**修复**: 在 CMAKE_C_FLAGS 中添加 `-Wno-deprecated-declarations`。

### 4. 系统 API 差异

| API | Linux | macOS | 修复 |
|-----|-------|-------|------|
| `fdatasync` | 可用 | 不可用（POSIX 可选） | `fsync` 回退（`#ifdef __APPLE__`） |
| `SOCK_NONBLOCK` | 可用 | 不可用 | `fcntl(fd, F_SETFL, O_NONBLOCK)` 回退 |
| `pthread_barrier_t` | 可用 | 不可用 | 替换为跨平台的 `uv_barrier_t` |
| `CLOCK_MONOTONIC` | 可用 | 可用 | 无需更改 |

macOS SDK 弃用了 `sprintf`—cJSON 使用了它。该问题被提升为 `-Werror,-Wdeprecated-declarations` 错误。

**修复**: 在 CMAKE_C_FLAGS 中添加 `-Wno-deprecated-declarations`。

### 5. `pthread_barrier_t` 不可用（阻塞项）

macOS 未实现 `pthread_barrier_t`（POSIX 可选功能）。
csilk 在 `src/core/server.c` 中使用它进行多 worker 线程同步。

**影响**：多 worker 模式（`worker_threads > 1`）在 macOS 上无法工作。

## pthread_barrier_t 的解决方案

| 方案 | 工作量 | 风险 |
|-----|:------:|:----:|
| **A:** 使用 `pthread_mutex` + `pthread_cond` 替换 | 2-3 天 | 低 |
| **B:** 使用 `os_unfair_lock` + `dispatch_semaphore`（仅 Darwin） | 1-2 天 | 中等 — 不可移植 |
| **C:** 限制 macOS 仅单 worker 模式 | 1 小时 | 低 — 功能受限 |
| **D:** 使用 libuv 屏障等价物（`uv_barrier_t`） | 1 天 | 低 — libuv 提供跨平台屏障 |

## 建议：方案 D（uv_barrier_t）

libuv 1.45+ 提供 `uv_barrier_t` / `uv_barrier_init` / `uv_barrier_wait` /
`uv_barrier_destroy`。由于 csilk 已依赖 libuv v1.48，这是最可移植和可维护的解决方案。

**实现**：
1. 将 `pthread_barrier_t` 替换为 `uv_barrier_t`
2. 将 `pthread_barrier_init` → `uv_barrier_init`
3. 将 `pthread_barrier_wait` → `uv_barrier_wait`
4. 将 `pthread_barrier_destroy` → `uv_barrier_destroy`

**受影响文件**：`src/core/server.c`（第 589、670、800 行附近共 3 处）

## 其他注意事项

### Homebrew 路径差异（Intel vs Apple Silicon）

- Intel：`/usr/local/`
- Apple Silicon：`/opt/homebrew/`

CMakeLists.txt 的 `find_library` 会处理此问题，但 CI 必须有条件地设置 `CMAKE_PREFIX_PATH`。

### CI 矩阵建议

将 pthread_barrier_t 替换为 uv_barrier_t 后，将 macOS 添加到 CI：
```yaml
os: [ubuntu-24.04, macos-14]
# Ubuntu 使用 GCC（默认），macOS 使用 Homebrew LLVM
```

## 结论

**已实现** — macOS 14 ARM64 现已在 CI 矩阵中（Ubuntu 24.04 + macos-14）。
所有阻塞项已解决：`pthread_barrier_t` → `uv_barrier_t`，`fdatasync` → `fsync`，
`SOCK_NONBLOCK` → `fcntl` 回退，ASan 误报已抑制。
