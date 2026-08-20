# Server Stats Aggregation & Data Race Elimination Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate data race in `csilk_server_get_stats()` by converting worker pool counters into relaxed atomics, maintaining worker-local active connection counts, and performing lock-free snapshot aggregation across workers.

---

### Task 1: Update `worker_pool_t` and Connection Pool Hot Path

**Files:**
- Modify: `src/core/internal/srv_internal.h`
- Modify: `src/core/server/connection_pool.c`
- Modify: `src/core/server/server_lifecycle.c`

- [x] **Step 1: Convert `client_pool_count`, `active_connections`, `arena_pool_count`, `read_buf_counts` to `_Atomic(int)` in `src/core/internal/srv_internal.h`**

- [x] **Step 2: Update `pool_get` and `pool_put` in `src/core/server/connection_pool.c` to use relaxed atomic operations and update worker `active_connections`**

- [x] **Step 3: Update `csilk_server_get_stats` in `src/core/server/server_lifecycle.c` to aggregate worker-local atomic stats**

- [x] **Step 4: Commit Task 1**
```bash
git add src/core/internal/srv_internal.h src/core/server/connection_pool.c src/core/server/server_lifecycle.c
git commit -m "feat(server): ✨ implement worker-local stats and lock-free snapshot aggregation"
```

---

### Task 2: Implement Concurrent Stats Benchmark & TSAN Stress Test

**Files:**
- Create: `tests/core/test_server_stats_bench.c`
- Modify: `cmake/tests.cmake`

- [x] **Step 1: Write `tests/core/test_server_stats_bench.c`**
  Concurrent test: 8 worker threads continuously acquiring/releasing connections while 4 reader threads continuously call `csilk_server_get_stats()`. Benchmark query latency and throughput.

- [x] **Step 2: Register in `cmake/tests.cmake`**

- [x] **Step 3: Build and run test under Default and TSAN builds**

- [x] **Step 4: Commit Task 2**
```bash
git add tests/core/test_server_stats_bench.c cmake/tests.cmake
git commit -m "test(server): ✅ add concurrent server stats benchmark and TSAN verification"
```

---

### Task 3: Full Verification & Code Formatting

- [x] **Step 1: Run full unit test suite via `ctest`**
- [x] **Step 2: Run full TSAN test suite**
- [x] **Step 3: Run code formatting**
- [x] **Step 4: Final commit**
