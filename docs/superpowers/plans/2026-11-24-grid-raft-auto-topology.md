# 分布式任务网格与 Raft 集群拓扑自愈闭环实施计划 (Grid ↔ Raft Auto-Topology Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 打通 Multi-Agent 任务网格（`csilk_wf_grid`）与 Raft 状态机的事件闭环，实现集群动态增删节点时的 Peer 拓扑自动同步，以及节点下线时的自动任务重平衡与租约自愈。

**Architecture:** 在 Raft 共识层暴露轻量级无锁拓扑事件监听器（`csilk_raft_set_topology_cb`）；在 Grid 层实现 `csilk_wf_grid_sync_raft_topology`，自动拉取存量成员并注册动态事件回调。在节点加入时毫秒级登记 Peer 窃取池，在节点下线时自动触发 `csilk_wf_grid_rebalance` 回收任务；同时维持 5 秒单调时钟租约作为网络异常兜底防线。

**Tech Stack:** C23, Raft Joint Consensus, POSIX Threads/Sync (`csilk_mutex_t`), Single-Writer Multi-Reader (SWMR), CMake, CTest.

**Spec:** [docs/superpowers/specs/2026-11-24-grid-raft-auto-topology-design.md](file:///data/home/quintin/workspace/source/c/csilk/docs/superpowers/specs/2026-11-24-grid-raft-auto-topology-design.md)

## Global Constraints

- **Language Standard**: C23 (`CMAKE_C_STANDARD 23`), keywords `nullptr`, `bool`, `constexpr` are built-in.
- **ABI Compatibility**: Strict SOVERSION 1 backward compatibility (`scripts/check_abi_compatibility.sh`), all new exported functions prefixed with `csilk_*`.
- **Concurrency & Deadlock Prevention**: One-way dependency. Grid listens to Raft. Raft must never hold a Grid mutex, and callbacks must be fired without holding long consensus locks.
- **Formatting**: Strictly compliant with LLVM 18 `clang-format` (`cmake --build build --target check-format`).
- **Declaration Placement**: Header `#include` directives and global/file `static` variables strictly placed before function definitions.

## Review Focus

1. **Deadlock in Raft callback**: Ensure `node->mutex` is not held during the invocation of `topology_cb` so that the Grid callback can freely acquire `grid->mutex` without lock inversion.
2. **Double rebalance safety**: Calling `csilk_wf_grid_rebalance` concurrently or multiple times for the same `failed_node_id` must be idempotent and thread-safe.
3. **Lease expiration timestamp integrity**: Monotonic time (`_get_monotonic_ms`) must handle rollover and 0-timestamps correctly.
4. **Memory lifecycle on teardown**: `csilk_wf_grid_free` must safely unregister from Raft and free all peer records.
5. **Pruned peer notification in Joint Consensus Phase 2**: Nodes removed in Phase 2 finalization must reliably trigger `CSILK_RAFT_TOPOLOGY_NODE_LEFT`.

---

## Tasks

### Task 1: Raft 拓扑事件监听器基础设施 (Raft Topology Event Listener Infrastructure)

**Files:**
- Modify: `include/csilk/messaging/raft.h`
- Modify: `src/messaging/raft_internal.h`
- Modify: `src/messaging/raft_membership.c`
- Test: `tests/messaging/test_raft_membership.c`

- [ ] **Step 1: 在 `include/csilk/messaging/raft.h` 中声明拓扑事件类型与监听器接口**
  - 定义 `csilk_raft_topology_event_t`（`CSILK_RAFT_TOPOLOGY_NODE_JOINED = 1`, `CSILK_RAFT_TOPOLOGY_NODE_LEFT = 2`, `CSILK_RAFT_TOPOLOGY_CONF_STABLE = 3`）。
  - 定义回调原型 `csilk_raft_topology_cb_t`。
  - 声明 `int csilk_raft_set_topology_cb(csilk_raft_node_t* node, csilk_raft_topology_cb_t cb, void* user_data);`。

- [ ] **Step 2: 在 `src/messaging/raft_internal.h` 的 `struct csilk_raft_node_s` 中增加监听器字段**
  - 添加 `csilk_raft_topology_cb_t topology_cb;` 与 `void* topology_user_data;`。

- [ ] **Step 3: 在 `src/messaging/raft_membership.c` 中实现注册函数与事件通知**
  - 实现 `csilk_raft_set_topology_cb`。
  - 在 `csilk_raft_node_add_peer` 和 `csilk_raft_add_node` 提交时触发 `CSILK_RAFT_TOPOLOGY_NODE_JOINED`。
  - 在 Phase 2 finalization 成员修剪循环中触发 `CSILK_RAFT_TOPOLOGY_NODE_LEFT`。
  - 在 `node->membership.state = CSILK_RAFT_CONF_STABLE` 时触发 `CSILK_RAFT_TOPOLOGY_CONF_STABLE`。
  - 确保事件触发时复制轻量级参数，不在持有 `node->mutex` 时阻塞用户回调。

- [ ] **Step 4: 在 `tests/messaging/test_raft_membership.c` 中增加拓扑事件单元测试**
  - 注册拓扑回调，记录触发的事件与 peer ID。
  - 执行 `csilk_raft_add_node` 与 `csilk_raft_remove_node`，断言事件依次按序正确触发。

- [ ] **Step 5: 运行测试并提交**
  - 执行 `ctest --test-dir build -R test_raft_membership --output-on-failure`。
  - 提交: `git commit -m "feat(raft): 🎯 implement dynamic topology change event listener interface"`。

---

### Task 2: Grid 拓扑自发现与动态重平衡 (Grid Topology Auto-Discovery & Dynamic Rebalance)

**Files:**
- Modify: `include/csilk/app/wf_grid.h`
- Modify: `src/workflow/wf_grid.c`
- Test: `tests/workflow/test_wf_grid.c`

- [ ] **Step 1: 在 `include/csilk/app/wf_grid.h` 中声明网格同步与 Peer 查询接口**
  - 声明 `int csilk_wf_grid_sync_raft_topology(csilk_wf_grid_t* grid);`。
  - 声明 `size_t csilk_wf_grid_peer_count(const csilk_wf_grid_t* grid);`。
  - 声明 `bool csilk_wf_grid_has_peer(const csilk_wf_grid_t* grid, const char* peer_id);`。

- [ ] **Step 2: 在 `src/workflow/wf_grid.c` 中实现 Peer 动态表与 Raft 回调处理**
  - 在 `struct csilk_wf_grid_s` 中增加跟踪的 Peer 列表 `char peer_ids[CSILK_WF_GRID_MAX_PEERS][64]; size_t peer_count;`。
  - 实现回调 `_on_raft_topology_event`：
    - `NODE_JOINED`：若非自身节点且不存在，则加入 Peer 列表；
    - `NODE_LEFT`：从 Peer 列表中移除，并立即调用 `csilk_wf_grid_rebalance(grid, peer_id)`。
  - 实现 `csilk_wf_grid_sync_raft_topology`：
    - 检查 `grid->raft`；
    - 调用 `csilk_raft_get_membership` 读取已有配置并填充 Peer 列表；
    - 调用 `csilk_raft_set_topology_cb(grid->raft, _on_raft_topology_event, grid)` 注册动态监听。
  - 在 `csilk_wf_grid_free` 中如果绑定了 `grid->raft`，调用 `csilk_raft_set_topology_cb(grid->raft, NULL, NULL)` 注销回调。

- [ ] **Step 3: 在 `tests/workflow/test_wf_grid.c` 中增加网格与 Raft 拓扑同步测试**
  - 创建虚拟 Raft 节点，网格绑定后调用 `csilk_wf_grid_sync_raft_topology`。
  - 模拟 `NODE_JOINED` 与 `NODE_LEFT`，验证 Peer 计数以及任务重平衡回收。

- [ ] **Step 4: 运行测试并提交**
  - 执行 `ctest --test-dir build -R test_wf_grid --output-on-failure`。
  - 提交: `git commit -m "feat(workflow): 🎯 implement Grid topology auto-discovery and auto-rebalance on node departure"`。

---

### Task 3: 集群端到端自愈集成测试 (Cluster End-to-End Self-Healing Integration Test)

**Files:**
- Create: `tests/workflow/test_wf_grid_raft.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: 编写 `tests/workflow/test_wf_grid_raft.c` 集成测试套件**
  - 场景 1：构建 3 节点真实 Raft 集群（Node A, Node B, Node C），分别绑定对应的 Grid 实例；
  - 场景 2：调用 `csilk_wf_grid_sync_raft_topology`，验证各节点互相自动发现为 Peer；
  - 场景 3：在 Node A 注入工作流任务，Node B 通过 `csilk_wf_grid_steal_and_execute` 成功窃取任务，租约持有者变为 Node B；
  - 场景 4：通过 `csilk_raft_change_membership_wait` 从集群移除 Node B；
  - 场景 5：验证 Node A 自动接收到 Node B 离开事件，自动触发 `rebalance`，窃取的未完成任务立即在 Node A 重置可用，并被 Node C 重新窃取执行；
  - 场景 6：网络分区与租约超时测试：模拟网络隔离导致离开事件未达，验证 5 秒租约过期后自动解冻。

- [ ] **Step 2: 在 `cmake/tests.cmake` 注册 `test_wf_grid_raft`**
  - 挂载至 `workflow` 测试套件，链接 `csilk_shared` 或 `csilk_static`。

- [ ] **Step 3: 运行集成测试并提交**
  - 执行 `ctest --test-dir build -R test_wf_grid_raft --output-on-failure`。
  - 提交: `git commit -m "test(workflow): 🎯 add end-to-end Grid and Raft auto-topology self-healing integration tests"`。

---

### Task 4: 格式化、ABI 门禁与全量测试回归 (Format, ABI & Full Regression)

**Files:**
- All touched files

- [ ] **Step 1: 格式化代码**
  - 运行 `cmake --build build --target format`。
  - 校验 `cmake --build build --target check-format`。

- [ ] **Step 2: 验证 ABI 稳定性**
  - 运行 `scripts/check_abi_compatibility.sh`，确保 SOVERSION 1 兼容，0 missing symbols，所有新符号符合 `csilk_*`。

- [ ] **Step 3: 全量测试回归**
  - 运行 `ctest --test-dir build --output-on-failure`，确保全量测试 100% 绿色通过。
  - 提交: `git commit -m "chore(release): 🚀 complete Grid-Raft auto-topology self-healing milestone"`。
