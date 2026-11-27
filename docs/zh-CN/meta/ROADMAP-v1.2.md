# csilk v1.2.0 路线图

## 概述

v1.2.0 聚焦于**协议完整性**（HTTP/3、gRPC）、**会话抽象**（可插拔后端）和**生产强化**（安全审计、内存优化）。

## 阶段

### 阶段 1：HTTP/3 (QUIC) — ✅ 完成

| 任务 | 状态 |
|---|---|
| 1.1 ngtcp2 QUIC 传输层 | ✅ `src/protocols/http3.c` |
| 1.2 nghttp3 会话层 (QPACK) | ✅ `src/protocols/h3_session.c` |
| 1.3 TLS 1.3 + QUIC (`ngtcp2_crypto_ossl`) | ✅ `src/protocols/http3.c`（OpenSSL ≥ 3.2） |
| 1.4 共享 router/middleware | ✅ 通过 `csilk_server_t` / `csilk_app_t` 自动复用 |
| 1.5 测试 + E2E + fuzz + CI | ✅ `tests/protocols/test_h3.c`、`test_h3_worker.c`、`test_h3_e2e.c` |

**关键决策：**
- 优先使用系统 ngtcp2 v1.25.0 / nghttp3 v1.18.0
- `csilk_h3_state_t` 收口在 `src/core/internal/srv_internal.h`（不暴露至公开头文件）
- `CSILK_ENABLE_HTTP3=ON` CMake 选项（默认 OFF）

### 阶段 2：会话存储 — ✅ 完成

| 任务 | 状态 |
|---|---|
| 2.1 `csilk_session_store_t` 抽象 | ✅ `include/csilk/core/session_store.h` |
| 2.2 Redis 后端 (hiredis) | ✅ `src/middleware/session_store.c` |
| 2.3 MQ 后端 (WAL + raft) | ✅ `src/middleware/session_store.c` |
| 2.4 TTL 跨节点同步 + 测试 | ✅ `tests/middleware/test_session_store.c` |

**关键决策：**
- 进程本地后端使用 256 桶分片哈希表（FNV-1a）
- Redis 后端使用 `SETEX` 管理 TTL
- MQ 后端有本地表回退机制

### 阶段 3：gRPC — ✅ 完成

| 任务 | 状态 |
|---|---|
| 3.1 帧解析 + trailer | ✅ `src/protocols/grpc.c` |
| 3.2 双向流式 | ✅ `csilk_grpc_stream_t` |
| 3.3 metadata 映射 | ✅ `csilk_grpc_metadata_parse` |
| 3.4 测试 | ✅ `tests/protocols/test_grpc.c` |

**关键决策：**
- `csilk_grpc_frame_encode_ex`（避免与 `grpc_gateway.c` 冲突）
- 流式使用逐字节帧重组，动态缓冲区扩展
- 4 种流类型：unary, client-streaming, server-streaming, bidi-streaming

### 阶段 4：生产强化 — ✅ 完成

| 任务 | 状态 |
|---|---|
| 4.1 清理模拟 Stub | ✅ 已移除 DPDK/XDP/WASM 模拟 stub，统一执行 fail-closed (`ENOTSUP`) |
| 4.2 连接池内存优化 | ✅ 已落地（Keep-Alive 延迟 arena 分配与按需回收） |
| 4.3 热更新（三组件原子） | ✅ 已落地（`csilk_hot_update`，位于 `src/core/server/server_rcu.c`） |
| 4.4 安全审计 | ✅ 状态同步至 [SECURITY_ANALYSIS.md](../../../SECURITY_ANALYSIS.md) |

### 阶段 6：战略功能收敛与自底向上架构硬化 (v1.2.0-GA) — ✅ 完成

| 任务 | 状态 |
|---|---|
| 6.1 OpenTelemetry 2.0 AI Trace ↔ HTTP W3C Spans & 上下文自动埋点 (`csilk_ai_chat_ctx`) | ✅ `src/middleware/otlp_trace.c`、`src/drivers/ai/ai.c`、`include/csilk/drivers/ai.h` |
| 6.2 去中心化 Multi-Agent 任务网格 ↔ Workflow 引擎深度融合 (`csilk_wf_set_grid`, `csilk_wf_submit_grid_task`) | ✅ `src/workflow/wf_grid.c`、`include/csilk/app/workflow.h` |
| 6.3 WebAssembly (WAMR) 沙箱 Fuel 计量、App 中间件 (`csilk_app_use_wasm`) 与 Workflow 节点 (`csilk_wf_add_wasm_plugin_node`) | ✅ `src/core/plugin/wasm_plugin.c`、`src/app/app.c`、`src/workflow/wf_tools.c` |
| 6.4 全局消除线程不安全 `strtok` 并实现 SSRF 防护过滤引擎 (`csilk_security_*`) | ✅ `src/core/primitives/ssrf.c`、`include/csilk/core/security.h`、`src/drivers/ai/llama.c` |
| 6.5 三组件原子热更新统一执行器 (`csilk_hot_update`) | ✅ `src/core/server/server_rcu.c`、`include/csilk/core/server/hot_reload.h` |
| 6.6 空闲 Keep-Alive 连接 Arena 延迟分配与按需回收 | ✅ `src/core/server/connection_io.c`、`src/core/http/http1_pipeline.c` |
| 6.7 全量 ABI SOVERSION 2 兼容性验证（876 纯净导出，100% `csilk_*`） | ✅ `scripts/check_abi_compatibility.sh` |
| 6.8 全量测试回归套件 | ✅ `ctest`（Core-Only & Full） |

### 阶段 7：Windows — ⚠️ 不在范围内

**决策**：Windows 需要 IOCP（替代 io_uring）、NDIS/WFP（替代 AF_XDP）、pthread_barrier shim。估算 8 周工作量。不在 v1.2.0 范围内。

## 遗留项（v1.3 候选）

- [x] 1.3 TLS 1.3 + QUIC（`ngtcp2_crypto_ossl`，OpenSSL ≥ 3.2）— 已在 v1.2.0 落地
- [ ] 4.2 SSL 上下文共享 (TLS 1.3 Session Ticket 复用)
- [x] 4.2 P0：延迟 arena 分配与按需回收 (已在 v1.2.0-GA 落地)
- [x] 4.3 P0：三组件原子热更新实现 (`csilk_hot_update` 已在 v1.2.0-GA 落地)
- [x] 安全：`strtok` → `strtok_r`（已在 v1.2.0-GA 全面清除）
- [x] SSRF 防护（内网与云元数据过滤引擎已在 v1.2.0-GA 落地）

## 测试覆盖

- **总测试数**：Core-Only 160 / Full 263+（`ctest` 100% 通过）
- **v1.2.0 新增**：test_h3, test_h3_e2e, test_io_equivalence, test_session_store, test_grpc, test_otlp_ai, test_wf_grid, test_wasm_fuel, test_wf_grid_integration, test_app_wasm, test_ai_ctx_trace, test_ssrf, test_hot_update_atomic, test_lazy_arena
- **Fuzz 目标**：11 个 harness（Core 6 个 + Full 5 个）

## 文件清单

```
src/protocols/http3.c                 # QUIC 传输层 (ngtcp2 v1.25.0)
src/protocols/h3_session.c            # HTTP/3 会话层 (nghttp3 v1.18.0 + QPACK)
src/protocols/grpc.c                  # gRPC 帧、trailer、流式
src/middleware/session_store.c        # 会话存储 (local/Redis/MQ)
include/csilk/protocols/h3.h          # HTTP/3 公共 API
include/csilk/protocols/grpc.h        # gRPC 公共 API
include/csilk/core/session_store.h    # 会话存储 vtable
tests/protocols/test_h3.c             # HTTP/3 单元与流测试
tests/protocols/test_h3_e2e.c         # HTTP/3 UDP/QUIC 端到端集成测试
tests/protocols/test_grpc.c           # gRPC 测试
tests/middleware/test_session_store.c # 会话存储测试
tests/core/test_io_equivalence.c      # libuv 与 io_uring 等价性测试
```
