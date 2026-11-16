# 连接池内存优化 — v1.2.0

## 目标

< 1.5 MB RSS / 10K keep-alive 连接

## 现状分析

### 每 worker 内存布局

| 组件 | 大小 | 说明 |
|---|---|---|
| `csilk_client_t` | ~1.2 KB × 32 = 38 KB | 连接结构体 + SSL 上下文 |
| Arena pool | ~4 KB × 32 = 128 KB | 预分配 arena（4/16/64 KB 分层） |
| Event loop | ~8 KB | libuv/io_uring loop + 文件描述符 |
| Worker 结构 | ~2 KB | wp 结构体 + dispatch 队列 |
| **合计/worker** | **~170 KB** | |

### 10K 连接 / 16 worker 估算

- 10K 连接 / 16 worker = 625 连接/worker
- 每 worker 实际活跃 ~625 连接，但池只预分配 32 个（复用）
- 超出 32 个的连接有独立 SSL 上下文（~2 KB each）
- 估算：32 × 1.2 KB (pooled) + 593 × 2 KB (SSL) ≈ 1.2 MB / worker
- 16 workers 总计 ≈ 19 MB（远超目标）

## 优化方案

### 1. SSL 上下文共享（最大收益）

当前每个 `csilk_client_t` 独立持有 `SSL*`（~2 KB）。优化：

- **TLS 1.3 session resumption**：共享 SSL_CTX，per-connection 只存 session ticket（~100 bytes）
- **预期收益**：~2 KB → ~200 bytes / 连接（90% 减少）

### 2. Arena 按需分配

当前预分配 32 个 arena（~128 KB/worker）。优化：

- **延迟分配**：只在连接实际创建时分配 arena
- **arena 大小动态化**：小连接（< 4 KB body）用 4 KB arena，大连接按需扩展
- **预期收益**：~128 KB → ~16 KB / worker（75% 减少）

### 3. 连接结构体精简

`csilk_client_t` 当前 ~1.2 KB。优化：

- **冷热分离**：热数据（state, generation, loop）放 64 B cache line，冷数据（SSL ctx, timers）指针化
- **预期收益**：~1.2 KB → ~200 B 热 + 指针

### 4. 池大小自适应

当前 `CSILK_CLIENT_POOL_SIZE = 32` 固定。优化：

- **动态池**：根据实际活跃连接数动态扩展（max 256）
- **回收策略**：空闲超过 30s 的池条目释放
- **预期收益**：避免过度预分配

## 实现计划

| 优先级 | 优化项 | 预计工作量 | 预计收益 |
|---|---|---|---|
| P0 | SSL 上下文共享 | 2d | -80% 连接内存 |
| P0 | Arena 延迟分配 | 1d | -75% 预分配内存 |
| P1 | 连接结构体冷热分离 | 1d | -50% 热数据 |
| P2 | 池大小自适应 | 1d | 避免过度分配 |

**总计**：4 天，预期 RSS 从 ~19 MB → ~3 MB / 16 workers

## 验证

- `scripts/run_benchmarks.sh` 对比优化前后 RSS
- `test_pool_economics` 单元测试
- 10K 连接 stress test（`test_connection_stress`）
