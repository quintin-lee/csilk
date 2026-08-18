# ABI 稳定性与架构边界评估报告

> **更新日期**: 2026-08-18 | 评估 csilk 3 层 ABI 架构与不透明句柄封装

## 概述

**状态: 已完成** — 3 层 ABI 架构（Public API → Opaque Handle → Internal Implementation）已完整实现。

内部结构体定义（`csilk_ctx_s`、`csilk_server_s`、`csilk_router_s`、`csilk_app_s`、`csilk_group_s`、`csilk_mq_s`、`csilk_raft_s`、`csilk_wf_s`、`csilk_mcp_server_s`）严格封装在 `src/**_internal.h` 中，对公共 `include/` 目录完全隐藏。所有外部用户代码仅依赖稳定的不透明指针句柄与公共函数 API。

---

## 3 层 ABI 架构

```
Public API (include/csilk/*.h)
       │
       ▼
Opaque Handles (csilk_ctx_t, csilk_router_t, csilk_server_t, csilk_app_t, csilk_mq_t...)
       │
       ▼
Internal Implementation (*_internal.h, ctx_internal.h, router_internal.h, server_internal.h...)
```

---

## 当前状态

### 公共 API — 不透明前向声明
```c
typedef struct csilk_ctx_s        csilk_ctx_t;        // include/csilk/core/types.h
typedef struct csilk_server_s     csilk_server_t;     // include/csilk/core/types.h
typedef struct csilk_router_s     csilk_router_t;     // include/csilk/core/router.h
typedef struct csilk_app_s        csilk_app_t;        // include/csilk/app/app.h
typedef struct csilk_group_s      csilk_group_t;      // include/csilk/core/group.h
typedef struct csilk_mq_s         csilk_mq_t;         // include/csilk/messaging/mq.h
typedef struct csilk_mq_ctx_s     csilk_mq_ctx_t;     // include/csilk/messaging/mq.h
typedef struct csilk_raft_s       csilk_raft_t;       // include/csilk/messaging/raft.h
typedef struct csilk_wf_s         csilk_wf_t;         // include/csilk/app/workflow.h
typedef struct csilk_mcp_server_s csilk_mcp_server_t; // include/csilk/protocols/mcp.h
typedef struct csilk_db_pool_s    csilk_db_pool_t;    // include/csilk/drivers/db.h
```

### 内部实现头文件（对公共 `include/` 隐藏）
```
src/core/ctx/ctx_internal.h           — csilk_ctx_s, arena, header_map, request_id, 序列计数器
src/core/server/server_internal.h     — csilk_server_s, worker 线程池, 连接池管理
src/core/primitives/router_internal.h — csilk_router_s, trie 节点, SIMD 查找表
src/messaging/mq_internal.h           — csilk_mq_s, csilk_mq_ctx_s 环形缓冲区与 WAL
src/messaging/raft_internal.h         — csilk_raft_s 共识状态机与通道
src/workflow/wf_internal.h            — csilk_wf_s DAG 图执行引擎
src/protocols/mcp/mcp_internal.h      — csilk_mcp_server_s 工具与 JSON-RPC 分发器
src/drivers/db/db_internal.h          — csilk_db_pool_s 驱动句柄与连接池
```

### 第三方库与底层后端解耦
1. **OpenSSL 头文件解耦**：`include/csilk/core/hash.h` 采用 64-bit 内存对齐的 128 字节不透明缓存定义 `csilk_sha1_ctx` 与 `csilk_sha256_ctx`，公共头文件不再直接依赖 `<openssl/sha.h>`。
2. **底层 I/O 后端句柄解耦**：`include/csilk/core/context.h` 移除 `csilk/core/sys_io.h` 包含，内部 worker 请求钩子（`csilk_get_work_req`）下沉至 `src/core/ctx/ctx_internal.h`。
3. **Router 结构体完全封装**：`struct csilk_router_s` 定义下沉至 `router_internal.h`，防止外部调用者耦合 Trie 树结构与中间件数组容量。

---

## 质量验证

- [x] 所有 15 个内置中间件完全基于公共访问器 API 实现。
- [x] 172 个标准单测与 170 个 io_uring 单测全量通过（100% Passed）。
- [x] 示例代码已完全迁移至干净的公共 API。
- [x] CI 矩阵（Ubuntu/macOS/ARM64/ASAN/TSAN/Fuzzing/Clang-tidy）全量绿灯通过。
