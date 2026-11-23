# csilk v1.2.0 路线图

## 概述

v1.2.0 聚焦于**协议完整性**（HTTP/3、gRPC）、**会话抽象**（可插拔后端）和**生产强化**（安全审计、内存优化）。

## 阶段

### 阶段 1：HTTP/3 (QUIC) — ✅ 完成

| 任务 | 状态 |
|---|---|
| 1.1 ngtcp2 QUIC 传输层 | ✅ `src/protocols/http3.c` |
| 1.2 nghttp3 会话层 (QPACK) | ✅ `src/protocols/h3_session.c` |
| 1.3 TLS 1.3 + QUIC | ⏳ 需要 OpenSSL ≥ 3.2 |
| 1.4 共享 router/middleware | ✅ 通过 csilk_app_t 自动实现 |
| 1.5 测试 + fuzz + CI | ✅ `tests/protocols/test_h3.c` |

**关键决策：**
- 优先使用系统 ngtcp2 0.3.1 / nghttp3 1.18.0
- `csilk_quic_transport_t` vtable 定义在 `server.h`
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
| 4.1 DPDK 决策 | ✅ 保留 stub（`docs/en/meta/DPDK-DECISION.md`） |
| 4.2 连接池内存优化 | ✅ 方案文档（`docs/en/meta/CONNECTION-POOL-OPTIMIZATION.md`） |
| 4.3 热更新（三组件原子） | ✅ 方案文档（`docs/en/meta/HOT-UPDATE-PLAN.md`） |
| 4.4 安全审计 | ✅ `docs/en/meta/SECURITY-AUDIT-v1.2.md` |

### 阶段 6：战略功能收敛 (v1.2.0-GA) — ✅ 完成

| 任务 | 状态 |
|---|---|
| 6.1 OpenTelemetry 2.0 AI Trace ↔ HTTP W3C Spans & 上下文自动埋点 (`csilk_ai_chat_ctx`) | ✅ `src/middleware/otlp_trace.c`、`src/drivers/ai/ai.c`、`include/csilk/drivers/ai.h` |
| 6.2 去中心化 Multi-Agent 任务网格 ↔ Workflow 引擎深度融合 (`csilk_wf_set_grid`, `csilk_wf_submit_grid_task`) | ✅ `src/workflow/wf_grid.c`、`include/csilk/app/workflow.h` |
| 6.3 WebAssembly (WAMR) 沙箱 Fuel 计量、App 中间件 (`csilk_app_use_wasm`) 与 Workflow 节点 (`csilk_wf_add_wasm_plugin_node`) | ✅ `src/core/plugin/wasm_plugin.c`、`src/app/app.c`、`src/workflow/wf_tools.c` |
| 6.4 全量 ABI SOVERSION 1 兼容性验证（960 纯净导出，100% `csilk_*`） | ✅ `scripts/check_abi_compatibility.sh` |
| 6.5 全量测试回归套件（276 单测 100% 绿色） | ✅ `ctest` |

### 阶段 7：Windows — ⚠️ 不在范围内

**决策**：Windows 需要 IOCP（替代 io_uring）、NDIS/WFP（替代 AF_XDP）、pthread_barrier shim。估算 8 周工作量。不在 v1.2.0 范围内。

## 遗留项（v1.3 候选）

- [ ] 1.3 TLS 1.3 + QUIC（需要 OpenSSL ≥ 3.2）
- [ ] 4.2 P0：SSL 上下文共享 + 延迟 arena 分配
- [ ] 4.3 P0：三组件原子热更新实现
- [ ] 安全：`strtok` → `strtok_r`（mysql.c/redis.c）
- [ ] SSRF 防护（内网 IP 黑名单）

## 测试覆盖

- **总测试数**：276+（单元测试 100% 通过）
- **v1.2.0 新增**：test_h3, test_session_store, test_grpc, test_otlp_ai, test_wf_grid, test_wasm_fuel, test_wf_grid_integration, test_app_wasm, test_ai_ctx_trace
- **Fuzz 目标**：9 个 harness

## 文件清单

```
src/protocols/http3.c              # QUIC 传输层 (ngtcp2 0.3.1)
src/protocols/h3_session.c         # HTTP/3 会话层 (nghttp3 + QPACK)
src/protocols/grpc.c              # gRPC 帧、trailer、流式
src/middleware/session_store.c    # 会话存储 (local/Redis/MQ)
include/csilk/protocols/h3.h      # HTTP/3 公共 API
include/csilk/protocols/grpc.h    # gRPC 公共 API
include/csilk/core/session_store.h # 会话存储 vtable
tests/protocols/test_h3.c        # HTTP/3 测试
tests/protocols/test_grpc.c      # gRPC 测试
tests/middleware/test_session_store.c # 会话存储测试
docs/en/meta/DPDK-DECISION.md    # DPDK 评估
docs/en/meta/CONNECTION-POOL-OPTIMIZATION.md
docs/en/meta/HOT-UPDATE-PLAN.md
docs/en/meta/SECURITY-AUDIT-v1.2.md
docs/en/meta/WINDOWS-ASSESSMENT.md
```
