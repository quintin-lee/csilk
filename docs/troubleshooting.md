# 故障排查手册

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-27

本文档涵盖 csilk 框架构建、运行时常见问题及调试技巧。所有错误处理遵循 RFC 2119 规范：**MUST** 修复、**SHOULD** 调优、**MAY** 降级。环境基准：Linux Kernel 6.12.91 + GCC 13.2 + libuv 1.48.0 + OpenSSL 3.0+。

---

## 1. 构建错误

### 1.1 C23 编译器版本错误

**错误信息**：
```
error: 'constexpr' specifier is incompatible with C standards before C23
error: 'nullptr' undeclared (first use in this function)
```

**原因**：CMake 默认 `CMAKE_C_STANDARD` 为 11，未升级至 C23（GCC 13+ / Clang 19+）

**解决**：
```bash
# 检查编译器版本
gcc --version  # MUST ≥ 13.0
clang --version  # MUST ≥ 19.0

# CMake 配置时强制指定 C23
cmake -B build -S . -DCMAKE_C_STANDARD=23
```

**MUST NOT**: 降级至 C11 — 框架依赖 `static constexpr` 和 `nullptr`。

### 1.2 libyaml 找不到

**错误信息**：
```
CMake Error: Could NOT find libyaml
  Hint: Install libyaml-dev (>= 0.2.0)
```

**原因**：系统未安装 libyaml 库

**解决**：
```bash
# Debian/Ubuntu
sudo apt-get update
sudo apt-get install libyaml-dev

# 指定版本（MUST ≥ 0.2.0）
sudo apt-get install libyaml-dev=0.2.11-4build1
```

**验证**：
```bash
pkg-config --modversion libyaml  # 应输出 0.2.x
```

### 1.3 zlib / OpenSSL 依赖缺失

**错误信息**：
```
CMake Error: zlib library not found
CMake Error: Could NOT find OpenSSL
```

**解决**：
```bash
# 完整依赖安装
sudo apt-get install libyaml-dev libssl-dev zlib1g-dev libcurl4-openssl-dev libpq-dev libsqlite3-dev
```

**版本要求**：
| 依赖 | 最低版本 | 说明 |
|:-----|:--------:|:-----|
| OpenSSL | 1.1.1 | TLS 1.3 必需 |
| libcurl | 7.80.0 | AI 驱动 HTTP 传输 |
| libpq | 13.0 | PostgreSQL 驱动 |
| sqlite3 | 3.20.0 | 嵌入式数据库驱动 |
| zlib | 1.2.0 | Gzip 压缩 |

### 1.4 CMake FetchContent 失败

**错误信息**：
```
Could not download 'https://github.com/libuv/libuv.git'
fatal: could not read Username for 'https://github.com/...'
```

**原因**：网络访问受限或 Git 认证失败

**解决**：
```bash
# 方案 1: 手动安装依赖
sudo apt-get install libuv1-dev libuv1-dev  # libuv v1.48.0+

# 方案 2: 配置 Git 认证（HTTPS）
git config --global credential.helper store

# 方案 3: 使用镜像源（中国大陆）
export HTTP_PROXY=http://your-proxy:port
export HTTPS_PROXY=http://your-proxy:port
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
```

---

## 2. 运行时错误

### 2.1 内存泄漏（ASan 报告）

**错误信息**：
```
==PID==ERROR: AddressSanitizer: detected heap-use-after-free
    #0 0x... in csilk_router_free
    #1 0x... in main
```

**原因**：未在所有错误路径释放资源

**定位**：
```bash
# 启用 ASan
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DUSE_ASAN=ON
make -C build -j$(nproc)
./build/example_server

# 查看详细堆栈
export ASAN_OPTIONS=detect_leaks=1:symbolize=1:abort_on_error=1
```

**常见泄漏点**：
| 模块 | 泄漏场景 | 修复方法 |
|:-----|:--------:|---------|
| `csilk_router_t` | 失败路径未调用 `router_free()` | 确保所有错误分支释放资源 |
| `csilk_server_t` | 优雅关闭前调用 `server_free()` | 使用 `csilk_server_stop()` 后再释放 |
| `csilk_arena_t` | 连接关闭未调用 `arena_reset()` | 在 `csilk_ctx_cleanup()` 中重置 Arena |

### 2.2 TLS 握手失败

**错误信息**：
```
SSL_ERROR_SSL: error:1408F10B:SSL routines:ssl3_get_record:wrong version number
```

**原因**：TLS 1.2 / TLS 1.1 不再受支持，必须使用 TLS 1.3

**解决**：
```c
// 配置 TLS 版本
csilk_server_config_t config = {
    .enable_tls = 1,
    .tls_min_version = TLS_VERSION_1_3,  // MUST be TLS 1.3
};
csilk_server_set_config(server, &config);

// 验证 TLS 版本
curl -V --tlsv1.3 https://localhost:8080/healthz
```

**MUST**: 生产环境 `tls_min_version` 设置为 `TLS_VERSION_1_3`。**SHOULD NOT**: 降级至 TLS 1.2（已存在安全漏洞）。

### 2.3 多 Worker 数据竞争

**错误信息**：
```
uv_accept: Assertion 'server->loop == client->loop' failed
```

**原因**：多 Worker 模式下连接池无锁保护

**解决**：
```c
// 确保 worker_threads ≥ 1
csilk_server_config_t config = {
    .worker_threads = 16,  // MUST = 物理 CPU 核心数
    .enable_cpu_affinity = 1,
};
csilk_server_set_config(server, &config);

// 检查是否启用了多 Worker
grep "CSILK_USE_MULTI_WORKER" build/CMakeCache.txt
# 应输出 CSILK_USE_MULTI_WORKER:BOOL=ON
```

---

## 3. 性能退化

### 3.1 吞吐量未达到预期

**症状**：单线程 QPS < 30K（基线 50K）

**排查**：
```bash
# 1. 检查编译优化等级
grep CMAKE_BUILD_TYPE build/CMakeCache.txt
# MUST 输出 Release

# 2. 检查 SIMD 是否启用
grep CSILK_HAS_AVX2 build/CMakeCache.txt
# MUST 输出 CSILK_HAS_AVX2:BOOL=ON

# 3. 检查系统参数
sudo sysctl net.core.somaxconn
# MUST ≥ 65535

# 4. 检查 ulimit
ulimit -n
# MUST ≥ 100000

# 5. 检查 CPU 频率（是否降频）
cat /proc/cpuinfo | grep "cpu MHz"
```

**常见瓶颈**：
| 瓶颈 | 现象 | 解决 |
|:-----|:----:|------|
| 编译优化等级 | `-O2` | 升级至 `-O3` |
| SIMD 未启用 | 路由延迟 ~100ns | 手动 `cmake -DCSILK_AVX512=ON` |
| 系统参数过小 | `somaxconn=128` | 设置 `net.core.somaxconn=65535` |
| ulimit 过小 | 连接数受限 | `ulimit -n 100000` |
| CPU 降频 | 频率 < 3GHz | 检查散热和电源策略 |

### 3.2 P99 延迟抖动大

**症状**：P99 延迟 > 10ms，基准 5ms

**排查**：
```bash
# 1. 使用 perf 分析延迟热点
sudo perf top -g --stdio

# 2. 检查 CPU 缓存命中率
perf stat -e cache-references,cache-misses ./example_server
# L3 cache miss rate 应 < 5%

# 3. 检查内存分配频率
perf record -e kmem:alloc,kmem:free -g ./example_server
perf report

# 4. 检查锁竞争
perf stat -e lock:,context-switches ./example_server
```

**常见原因**：
| 原因 | 修复 |
|:-----|------|
| Arena 扩容频率高 | 增大 Arena 块大小至 16KB+ |
| 全局锁竞争 | 检查 Session 链表是否加锁 |
| CPU 缓存未命中 | 启用 CPU 亲和性绑定 |
| 系统调用开销 | 启用 `tcp_nodelay` |

---

## 4. WebSocket / SSE 问题

### 4.1 WebSocket 握手失败

**错误信息**：
```
HTTP 400 Bad Request: Invalid Upgrade header
```

**原因**：请求缺少 `Upgrade: websocket` 或 `Connection: Upgrade` 头

**解决**：
```c
// 验证 WebSocket 端点是否正确
curl -i -H "Connection: Upgrade" -H "Upgrade: websocket" \
    http://localhost:8080/ws

// 检查 ALPN 协商
curl -v --http2 https://localhost:8080/ws
# MUST 输出 ALPN negotiated: h2
```

**MUST**: WebSocket 连接必须使用 HTTP/2 或 HTTP/1.1。**SHOULD NOT**: 使用 HTTP/1.0（不支持 Upgrade 头）。

### 4.2 SSE 流中断

**症状**：SSE 事件 5s 后断开

**原因**：`keep_alive_timeout_ms` 过短

**解决**：
```c
// 延长 keep-alive 超时
csilk_server_config_t config = {
    .keep_alive_timeout_ms = 60000,  // 60s
};
csilk_server_set_config(server, &config);

// 或在 SSE 连接中主动发送心跳
csilk_sse_send(c, "event: heartbeat\ndata: ping\n\n");
```

---

## 5. 调试技巧

### 5.1 启用详细日志

```c
csilk_server_config_t config = {
    .log_level = CSILK_LOG_LEVEL_DEBUG,  // 输出所有日志
    .log_to_file = 1,
    .log_file = "/var/log/csilk/server.log",
};
```

### 5.2 使用 GDB 多线程调试

```bash
# 运行程序并附加 GDB
gdb ./example_server
(gdb) set pagination off
(gdb) set follow-fork-mode parent
(gdb) target remote :1234

# 监控所有线程
(gdb) info threads
(gdb) thread apply all bt
```

### 5.3 使用 strace 追踪系统调用

```bash
# 限制输出频率（避免刷屏）
strace -c -f -T ./example_server
# 输出：
#     time     seconds  usecs/call     calls     errors     syscall
#    ...
    5.123456   5.123456       123.45       12345          0  epoll_wait

# 追踪特定函数
strace -e trace=network,socket,bind,listen -f ./example_server
```

### 5.4 使用 Valgrind 内存检查

```bash
# 检查内存泄漏
valgrind --leak-check=full --show-leak-kinds=all ./example_server

# 检查内存越界
valgrind --track-origins=yes --show-leak-kinds=all ./example_server

# 检查线程竞争
valgrind --tool=helgrind ./example_server
```

---

## 6. 常见问题速查表

| 错误 | 原因 | 解决 |
|:-----|:----:|------|
| `C23 constexpr` 错误 | 编译器 < GCC 13/Clang 19 | 升级编译器或设置 `CMAKE_C_STANDARD=23` |
| `libyaml not found` | 未安装 libyaml-dev | `sudo apt-get install libyaml-dev` |
| `SSL_ERROR` | TLS 1.2 不再支持 | 使用 TLS 1.3 (`tls_min_version = TLS_VERSION_1_3`) |
| `uv_accept assertion` | 多 Worker 连接池竞态 | 确保设置 `worker_threads ≥ 1` |
| `heap-use-after-free` | 资源泄漏 | 启用 ASan 定位泄漏点 |
| `socket: Resource temporarily unavailable` | ulimit 过小 | `ulimit -n 100000` |
| `Too many open files` | ulimit 过小 | `ulimit -n 100000` |

---

## 7. 获取帮助

**问题已解决但需要进一步指导**？提交 Issue 时请包含：

1. **错误完整堆栈**（`ASAN_OPTIONS` 启用时）
2. **系统环境**（`uname -a`、`gcc --version`、`openssl version`）
3. **复现步骤**（最小化示例代码）
4. **日志输出**（完整 `server.log` 或终端输出）

**提交模板**：
```markdown
## 问题

[简要描述错误]

## 错误信息

```
[完整的错误堆栈]
```

## 环境信息

```bash
uname -a
gcc --version
cmake --version
openssl version
```

## 复现步骤

```c
// 最小化代码示例
```

## 预期行为

[期望的正确结果]

## 实际行为

[实际的错误输出]
```
