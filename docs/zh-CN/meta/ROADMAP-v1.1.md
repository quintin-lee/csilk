# csilk v1.1.0 演进规划

> **基线版本**: `1.0.0 LTS` | **规划日期**: `2026-11-15`
> [English](../../en/meta/ROADMAP-v1.1.md) | [中文](ROADMAP-v1.1.md)

---

## 零、v1.1.0 核心定位

**主题**：实验性 Stub 转正 + 覆盖率攻坚 + 并发安全修补。

v1.0.0 LTS 承诺"不在稳定性范围内"的 stub 模块（WASM、AF_XDP、DPDK）在 1.1 中提供**真实实装路径**，
同时补齐已知并发缺陷、提升全库行覆盖率至 80%+，为 v1.2 HTTP/3 实装铺平架构基础。

---

## 一、当前 v1.0.0 遗留问题清单（已核实）

| 编号 | 模块 | 问题描述 | 严重度 |
|------|------|----------|--------|
| L1 | `wasm_plugin.c` | `fuel_limit` 在多次调用后耗尽，插件永久 disabled（设计缺陷） | P1 |
| L2 | `wasm_vm.c` | `csilk_wasm_plugin_exec` 为模拟 JSON 返回，无真实字节码解释器 | P0（实装目标） |
| L3 | `session.c` | 全链表 O(n) 遍历，>10K 活跃 session 性能劣化；无过期清扫调度器 | P2 |
| L4 | `session.c` | 进程内 store，多节点部署时 session 不共享 | P2（v1.2 目标） |
| L5 | `otlp_trace.c` | `g_otlp_buffer` 固定 2048 span 环形缓冲，高 QPS 下溢出丢 trace | P2 |
| L6 | `wf_cluster_sm.c` | 静态 `s_bindings[32]` 硬编码上限，超过 32 个 workflow↔Raft 绑定无法注册 | P2 |
| L7 | `af_xdp.c` | 模拟 RX/TX 游标，无真实 kernel bypass；`io_perf_probe.c` 仅检查 `geteuid()==0` | P0（实装目标） |
| L8 | `CSILK_ENABLE_EXPERIMENTAL_STUBS` | 默认 `ON`，与 1.0.0 稳定性承诺冲突；需默认改为 `OFF` | P1 |
| L9 | 覆盖率 | 全库 69%（12,609/18,281 行）；`otlp_trace`、`wf_cluster_sm`、`vector_hnsw` 持久化路径未覆盖 | P1 |

---

## 二、v1.1.0 分阶段计划

### Phase 1：WASM 真实实装（8-10 周）

**目标**：用 WAMR（WebAssembly Micro Runtime）替换 `wasm_vm.c` 模拟执行，
使 `csilk_wasm_plugin_execute` 能真正加载并运行 WebAssembly 字节码。

#### 1.1 依赖集成

- **FetchContent**：引入 [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
  （`-DWAMR_BUILD_INTERP=1` 解释器模式，无 JIT，嵌入式友好）。
- **CMake 选项**：`-DCSILK_ENABLE_WAMR=ON`（默认 `OFF`，保持可选编译）；
  当 `CSILK_ENABLE_WAMR=ON` 时，`CSILK_ENABLE_EXPERIMENTAL_STUBS` 强制 `OFF`。
- **模块归属**：新增 `cmake/wamr_fetch.cmake` 或在 `dependencies.cmake` 中追加 FetchContent 块。

#### 1.2 API 重构

- `csilk_wasm_plugin_t` 内部结构扩展：
  ```c
  struct csilk_wasm_plugin_s {
      csilk_mutex_t       mutex;
      csilk_wasm_config_t config;
      csilk_wasm_memory_t memory;
      uint8_t*            bytecode;
      size_t              bytecode_size;
      char                entry_point[64];
      /* WAMR 实装字段 */
      void*               wasm_exec_env;   /* wasi_exec_env 或 WAMR exec env */
      uint32_t            wasm_module_id;  /* WAMR 模块句柄 */
      bool                use_wamr;
  };
  ```
- `csilk_wasm_plugin_exec` 实装路径：
  1. 将 `bytecode` 载入 WAMR module（`wasm_loader_create` 或 `wasm_module_create`）。
  2. 建立 WASI host-imports：`fd_write`/`clock_time_get` 替换 `wasm_wasi.c` 常量 stub。
  3. 调用 `csilk_wasm_host_*` C-ABI 函数指针注册为 host imports。
  4. fuel 预算（`config.fuel_limit`）映射到 WAMR 的 instruction limit。
  5. **修复 L1**：fuel 耗尽 trap 后，重置 `config.fuel_limit` 到 `config` 初始值，
     使每次调用独立计算，不累计耗尽。

#### 1.3 热重载打通

- 新增 `csilk_wasm_plugin_hot_reload(plugin_t*, new_bytes, new_size)` API：
  - 旧模块通过 EBR 宽限期延迟销毁；
  - 新模块加载成功后，`g_mw_plugin` 指针原子切换；
  - 在途请求继续使用旧实例（`plugin->use_wamr` 标志保证一致性）。
- `hot_reload.c` 增加 `wasm_plugin` 类型分发钩子。

#### 1.4 测试与验证

- 新增 `tests/core/test_wasm_real.c`：
  - 编译示例 `.wasm`（wat2wasm）→ 载入 → 执行 `on_request` → 验证 host header 修改。
  - fuel 超限 trap 测试：循环 10 次后重新调用，fuel 应重置，不永久 disabled（**L1 回归**）。
  - WASI `fd_write` 输出捕获测试。
- Fuzz 目标扩展：`fuzz_wasm`（畸形 WASM 字节码 + WAMR 加载路径）。

**验收标准**：
- `-DCSILK_ENABLE_WAMR=ON` 下 `test_wasm_real` 全绿；
- `libcsilk.so` 导出符号表无新增内部 `_csilk_wamr_*` 泄漏（ABI 门禁通过）；
- 不启用 WAMR 时（默认），行为与 1.0.0 完全一致（零回归）。

---

### Phase 2：AF_XDP 真实内核旁路实装（4-6 周）

**目标**：`af_xdp.c` 从模拟游标替换为真实 `AF_XDP` socket syscall，
`io_perf_probe.c` 从 `geteuid` 判断改为内核特性探测。

#### 2.1 依赖与平台约束

- **平台**：Linux ≥ 5.3（AF_XDP 内核支持）+ 驱动必须支持 XSK（如 `virtio-net`、`ice`、`bnxt_en`）。
- **权限**：需要 `CAP_NET_RAW`（通常 root 或 `capsh --caps=+cap_net_raw+e`）。
- **CMake 选项**：`-DCSILK_ENABLE_AF_XDP=ON`（默认 `OFF`）；启用时 `CSILK_BYPASS_SOURCES` 中
  `af_xdp.c` / `af_xdp_zerocopy.c` 改为真实实现，`dpdk_pmd.c` 保持 stub 或移除。
- **内核 UMEM**：通过 `socket(AF_XDP, SOCK_RAW, 0)` 创建 UMEM；RX/TX ring 由内核管理，
  用户态仅通过 `umem_area` 直接读取帧数据（零拷贝）。

#### 2.2 实装内容

- `csilk_xdp_socket_new`：
  - 调用 `socket(AF_XDP)` + `setsockopt(AF_XDP_UMEM_REG)` 注册 UMEM。
  - 绑定 `ifname` + `queue_id`；失败时返回 `ENOTSUP` 并 log。
  - `fd` 字段存储真实 socket fd（当前为 `-1`）。
- `csilk_xdp_rx_burst`：
  - 从 `rx_ring` 读取 `XDP_RX_DESC`，映射到 UMEM 偏移，返回帧指针（**零拷贝**）。
  - 推进 `rx_head` 由内核通过 `XDP_RX_FILL` 管理（非用户态手动推进）。
- `io_perf_probe.c`：
  - 探测路径改为 `xsk_socket_get_features()` + `/sys/class/net/<ifname>/xsk` 目录存在性检查。
  - `has_xdp_zerocopy` 仅在真实 socket 创建成功时置 1。

#### 2.3 测试

- 新增 `tests/core/test_af_xdp.c`：
  - 无 root / 无 XSK 驱动环境：fail-closed 测试（验证 `ENOTSUP` 返回 + log）。
  - root + virtio-net（Docker 容器测试）：完整 RX/TX burst 路径覆盖。
- CI 新增 `af_xdp` job（Ubuntu 24.04 容器，root 权限）；本地开发无需 root 也能通过（stub 降级）。

**验收标准**：
- `-DCSILK_ENABLE_AF_XDP=ON` 下 root 环境 RX/TX burst 路径覆盖；
- 非 root 环境返回 fail-closed `ENOTSUP`，无 crash；
- `io_perf_probe.c` 在 root 环境下 `has_xdp_zerocopy=1` 仅在真实 socket 可用时成立。

---

### Phase 3：覆盖率攻坚（与 Phase 1/2 并行，4 周）

**目标**：全库行覆盖率从 69% → **80%+**。

#### 3.1 重点模块覆盖计划

| 模块 | 当前 | 目标 | 新增测试 |
|------|------|------|----------|
| `src/middleware/otlp_trace.c` | ~55% | 90% | 跨模块子 Span 串联集成测试；`traceparent` 注入/透传 |
| `src/middleware/otlp_exporter.c` | ~60% | 85% | 异步批量导出 + 背压 + HTTP 429 重试 |
| `src/workflow/wf_cluster_sm.c` | ~70% | 90% | 3 节点 Raft 快照恢复 + 命令重放幂等 |
| `src/drivers/vector/vector_hnsw.c` | ~80% | 95% | 二进制快照持久化 + 召回率 >95% 验证 |
| `src/middleware/ratelimit.c`（分布式） | ~71% | 85% | Redis 存储 mock + 分布式限流集成测试（CI 容器） |
| `src/core/plugin/wasm_plugin.c` | ~80% | 95% | Phase 1 新增测试覆盖真实 WAMR 路径 |
| `src/core/io/af_xdp.c`（真实） | 0%（stub） | 80% | Phase 2 新增测试 |
| `src/messaging/raft_snapshot.c` | ~75% | 90% | 快照安装 + 日志压缩截断集成测试 |

#### 3.2 覆盖率门禁

- `.gcovr` 配置：全库目标 80%；`CI` coverage job 在低于 78% 时 fail。
- 新增 `.gcovr` 排除：`flamegraph.c`、`redis_storage.c`、`workflow_debug.c`、`uring_vector.c`（已排除）。

**验收标准**：`ctest -E test_integration` 全绿 + gcovr 报告 ≥ 80%。

---

### Phase 4：并发安全与正确性修补（3 周）

#### 4.1 `session.c` 哈希化（修复 L3）

- 将全局 `session_store` 链表替换为 **分片哈希表**（1024 桶，每桶 mutex）：
  - `csilk_session_get(id)` → 哈希定位桶 → 桶内链表线性查找（典型 1-2 跳）。
  - 过期清扫改为后台 `uv_timer` 每 60s 运行一次，遍历各桶删除过期 session。
  - 内存上限：`csilk_session_set_max(s, N)` 超限触发 LRU 淘汰。
- 测试：`tests/middleware/test_session_concurrent.c`（4 线程 × 10K 并发 session 读写）。

#### 4.2 `otlp_trace.c` 动态扩容（修复 L5）

- `g_otlp_buffer` 从固定 2048 改为**动态分片环形缓冲**（每 worker 独立 1024 span，全局汇聚）。
- 高 QPS 溢出时：降级为采样（可配置采样率，默认 1/1000），而非静默丢弃全部 trace。
- 测试：`test_otlp_trace_overflow`（注入 5000 span，验证采样降级无 crash）。

#### 4.3 `wf_cluster_sm.c` 参数化绑定上限（修复 L6）

- `WF_CLUSTER_MAX_BINDINGS` 从硬编码 32 改为编译时选项 `CSILK_WF_CLUSTER_MAX_BINDINGS`（默认 128）。
- 超出上限时返回 `-E2BIG`，由调用方决策。
- 测试：`test_wf_cluster_sm_overflow`（注册 129 个绑定，验证第 129 个返回错误）。

#### 4.4 `CSILK_ENABLE_EXPERIMENTAL_STUBS` 默认值翻转（修复 L8）

- `cmake/options.cmake`：`option(CSILK_ENABLE_EXPERIMENTAL_STUBS ... OFF)` （默认 `OFF`）。
- 所有 stub 路径在 `OFF` 时 fail-closed（当前已实现 `ENOTSUP` 返回，无需额外改动）。
- 文档同步：README / CHANGELOG / AGENTS.md 更新"默认 fail-closed"说明。

**验收标准**：
- TSAN 全量测试（`USE_TSAN=ON`）零数据竞争报告；
- `session.c` 并发测试 10K 连接下 P99 延迟 < 1ms（对比链表 O(n) 劣化）。

---

### Phase 5：HTTP/3 架构铺垫（2 周）

**目标**：不完成 HTTP/3 实装（推迟至 v1.2），但为 ngtcp2/nghttp3 集成做好架构准备。

#### 5.1 依赖预研

- `cmake/dependencies.cmake`：追加可选 FetchContent 块（`-DCSILK_ENABLE_HTTP3=ON`，默认 `OFF`）：
  ```cmake
  if(CSILK_ENABLE_HTTP3)
    FetchContent_Declare(ngtcp2 GIT_REPOSITORY ... GIT_TAG v1.x)
    FetchContent_Declare(nghttp3 GIT_REPOSITORY ... GIT_TAG v1.x)
  endif()
  ```
- `src/protocols/h3.c`：新增 `csilk_h3_session_t`（ngtcp2_conn 句柄 + nghttp3 会话）
  内部结构定义，暂存于 `src/protocols/h3_internal.h`，`CSILK_ENABLE_HTTP3=ON` 时编译。

#### 5.2 测试基建

- 新增 `fuzz/fuzz_h3` 扩展：对现有 `csilk_h3_frame_encode/decode` 增加 ngtcp2 互操作 fuzz
  （提供已知正确帧序列作为 seed corpus）。
- 新增 `tests/protocols/test_h3_handshake.c`（需 ngtcp2 + OpenSSL QUIC）：
  - 仅在 `CSILK_ENABLE_HTTP3=ON` 且系统 OpenSSL ≥ 3.2 时编译运行。

**验收标准**：
- `-DCSILK_ENABLE_HTTP3=ON` 下编译通过（不要求测试通过）；
- `CSILK_ENABLE_HTTP3=OFF`（默认）零回归，`libcsilk.so` 符号表不变。

---

## 三、v1.1.0 完整任务分解表

| 任务 ID | 模块 | 内容 | 预估工时 | 依赖 |
|---------|------|------|---------|------|
| T1.1 | WASM | WAMR FetchContent + CMake 选项 | 3 天 | — |
| T1.2 | WASM | `wasm_plugin_t` 扩展 + WAMR 加载路径 | 5 天 | T1.1 |
| T1.3 | WASM | WASI host-imports 实装（替换常量 stub） | 4 天 | T1.2 |
| T1.4 | WASM | fuel 预算修复（L1）+ 热重载 API | 3 天 | T1.2 |
| T1.5 | WASM | `test_wasm_real` + `fuzz_wasm` + CI | 4 天 | T1.3 |
| T2.1 | AF_XDP | 真实 socket/UMEM 实装 | 5 天 | — |
| T2.2 | AF_XDP | `io_perf_probe.c` 内核特性探测 | 2 天 | T2.1 |
| T2.3 | AF_XDP | CI root 容器 job + 本地 fail-closed 测试 | 3 天 | T2.1 |
| T3.1 | 覆盖率 | `otlp_trace`/`otlp_exporter` 集成测试 | 5 天 | — |
| T3.2 | 覆盖率 | `wf_cluster_sm` 快照恢复测试 | 4 天 | — |
| T3.3 | 覆盖率 | `vector_hnsw` 持久化 + 召回率测试 | 4 天 | — |
| T3.4 | 覆盖率 | `ratelimit` 分布式路径 Redis mock 测试 | 3 天 | — |
| T3.5 | 覆盖率 | gcovr 门禁 80% + CI 配置 | 2 天 | T3.1-3.4 |
| T4.1 | session | 分片哈希表 + 过期清扫定时器 | 5 天 | — |
| T4.2 | otlp | 动态分片 + 采样降级 | 3 天 | — |
| T4.3 | wf_cluster | `CSILK_WF_CLUSTER_MAX_BINDINGS` 参数化 | 1 天 | — |
| T4.4 | cmake | `CSILK_ENABLE_EXPERIMENTAL_STUBS` 默认翻转 | 1 天 | T4.1-4.3 |
| T5.1 | HTTP/3 | ngtcp2/nghttp3 FetchContent + 内部结构定义 | 3 天 | — |
| T5.2 | HTTP/3 | `fuzz_h3` 扩展 + 条件编译测试骨架 | 3 天 | T5.1 |
| T6 | 文档 | ROADMAP / CHANGELOG / AGENTS.md 同步 | 2 天 | 全部 |

**总预估**：约 72 人天（≈ 14-16 周，可并行压缩至 **10-12 周**）。

---

## 四、版本节奏

```
v1.1.0-rc1（Phase 1 完成 + Phase 4 完成）
  └─ WASM WAMR 实装 + 并发安全修补 + 默认 fail-closed

v1.1.0-rc2（Phase 2 + Phase 3 完成）
  └─ AF_XDP 真实实装 + 覆盖率 ≥ 80%

v1.1.0（Phase 5 完成 + 全量 CI 绿）
  └─ HTTP/3 架构铺垫 + 全量发布（SOVERSION 保持 1）
```

---

## 五、ABI 兼容性承诺

v1.1.0 保持 `SOVERSION = 1`，以下 API 为**新增**（向后兼容）：
- `csilk_wasm_plugin_hot_reload()`
- `csilk_wasm_plugin_exec` 行为变更（fuel 修复）— 语义不变，仅修复缺陷
- `CSILK_WF_CLUSTER_MAX_BINDINGS` 编译时选项（默认 128，不影响二进制 ABI）
- `csilk_h3_session_t`（`CSILK_ENABLE_HTTP3=ON` 时新增，默认不编译）

**移除/变更**：
- `CSILK_ENABLE_EXPERIMENTAL_STUBS` 默认值翻转（编译时行为变化，非 ABI 破坏）
- `session.c` 内部 store 实现变更（公共 API `csilk_session_*` 不变）

---

## 六、与 v1.2.0 的边界

v1.1.0 **不**包含以下内容（留待 v1.2）：
- HTTP/3 完整实装（ngtcp2 + nghttp3 传输层）
- gRPC 原生双向流
- 集群 Session 共享（Redis + csilk_mq）
- DPDK 实装或正式移除
- Windows/MSVC 移植评估

---

## 七、实施进度追踪（截至 2026-11-15）

| 阶段 | 项目 | 状态 | 提交 |
|------|------|------|------|
| 4.1 | session.c 分片哈希表 + LRU 上限 | ✅ 完成 | `eca05613` |
| 4.2 | otlp_trace.c 动态分片 + 采样降级 | ✅ 完成 | `6bd35d50` |
| 4.3 | wf_cluster_sm.c 参数化绑定上限 | ✅ 完成 | `45fbd1aa` |
| 4.4 | CSILK_ENABLE_EXPERIMENTAL_STUBS 默认 OFF | ✅ 完成 | `2d6eff55` |
| 1.1 | WAMR 2.4.4 FetchContent + CMake 选项 | ✅ 完成 | `329ab773` |
| 1.2 | wasm_plugin_t 扩展 + WAMR 加载路径 | ✅ 完成 | `6895a6a6` |
| 1.3 | WASI host-imports 实装 | ✅ 完成 | `6895a6a6` |
| 1.4 | fuel 预算修复 + 热重载 API | ✅ 完成 | `6895a6a6` |
| 1.5 | test_wasm_real + fuzz_wasm + CI | ✅ 完成 | `5b95ac4e` |
| 2.1 | AF_XDP 真实 socket/UMEM/ring 实装 | ✅ 完成 | `1269527a` |
| 2.2 | io_perf_probe.c 内核特性探测 | ✅ 完成 | `3c37ffc4` |
| 2.3 | AF_XDP fail-closed 测试 | ✅ 完成 | `e8e68a05` |
| 3.1 | otlp_trace/otlp_exporter 覆盖率测试 | ✅ 完成 | `09e05a84` |
| 3.2 | wf_cluster_sm 快照恢复测试 | ✅ 完成 | `989583ba` |
| 3.3 | vector_hnsw 1000x128 recall >95% | ✅ 完成 | `56e2818c` |
| 3.4 | ratelimit 分布式 Redis mock 测试 | ✅ 完成 | `c8aed585` |
| 3.5 | gcovr 80% 门禁 + CI 配置 | ✅ 完成 | `16c429cf` |
| 5.1 | ngtcp2/nghttp3 FetchContent + 类型定义 | ✅ 完成 | `1170404e` |
| 5.2 | fuzz_h3 + 条件编译测试骨架 | ✅ 完成 | `acf337c7` |
