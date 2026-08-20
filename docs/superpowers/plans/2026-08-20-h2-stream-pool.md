# HTTP/2 Per-Connection Stream Pool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a per-connection stream context and arena pool in `src/core/http/h2_session.c` and `src/core/internal/srv_internal.h` to enable $O(1)$ stream acquisition, deterministic arena reuse via `csilk_arena_reset()`, and single-pass connection teardown.

**Architecture:** Extend `csilk_h2_stream_map_t` with a LIFO `free_list` of idle stream contexts. On stream creation, pop and reset an existing context/arena if available; on stream close, clean up context, reset arena, and push to `free_list` up to `pool_max` (64). Tear down both active and pooled streams on connection close.

**Tech Stack:** C23, nghttp2, Clang/GCC, AddressSanitizer, CTest, CMake.

---

### Task 1: Update `csilk_h2_stream_map_t` Definition

**Files:**
- Modify: `src/core/internal/srv_internal.h`

- [ ] **Step 1: Add pool fields to `csilk_h2_stream_map_t`**
Add `free_list`, `pool_count`, and `pool_max` to `csilk_h2_stream_map_t` in `src/core/internal/srv_internal.h`.

- [ ] **Step 2: Commit Task 1**
```bash
git add src/core/internal/srv_internal.h
git commit -m "feat(h2): ✨ add free_list, pool_count and pool_max to csilk_h2_stream_map_t"
```

---

### Task 2: Implement Stream Context & Arena Pooling in `h2_session.c`

**Files:**
- Modify: `src/core/http/h2_session.c`

- [ ] **Step 1: Update `_csilk_h2_stream_map_ensure_init`**
Initialize `map->free_list = NULL; map->pool_count = 0; map->pool_max = CSILK_H2_STREAM_POOL_MAX;`.

- [ ] **Step 2: Update `csilk_h2_get_or_create_stream` to acquire from `free_list`**
Pop from `map->free_list`, invoke `csilk_arena_reset(ctx->arena)`, reinitialize context via `_csilk_ctx_init()`.

- [ ] **Step 3: Update `csilk_h2_remove_stream` to recycle to `free_list`**
Invoke `csilk_ctx_cleanup(found)`, if `map->pool_count < map->pool_max`, reset arena with `csilk_arena_reset(found->arena)` and push to `free_list`.

- [ ] **Step 4: Update `csilk_h2_free_streams` to drain `free_list`**
Iterate through `free_list`, freeing all arenas (`csilk_arena_free`) and contexts (`free`).

- [ ] **Step 5: Commit Task 2**
```bash
git add src/core/http/h2_session.c
git commit -m "feat(h2): ✨ implement per-connection stream context and arena pooling"
```

---

### Task 3: Add Stream Pooling Verification & Scale Benchmarks

**Files:**
- Modify: `tests/core/test_h2_stream_bench.c`

- [ ] **Step 1: Add `test_h2_stream_pool_recycling()`**
Test that 100 consecutive stream open/close cycles reuse the exact same stream context pointers and arena instances without extra allocations.

- [ ] **Step 2: Add pool benchmark for 100 and 1,000 concurrent streams**
Measure acquire + release throughput with pooling vs cold allocation.

- [ ] **Step 3: Build and run test**
```bash
cmake --build build --target test_h2_stream_bench
./build/test_h2_stream_bench
```

- [ ] **Step 4: Commit Task 3**
```bash
git add tests/core/test_h2_stream_bench.c
git commit -m "test(h2): ✅ add stream pool recycling verification and throughput benchmarks"
```

---

### Task 4: Full Test Suite, ASAN Verification & Format

**Files:**
- All touched files

- [ ] **Step 1: Run full unit test suite**
```bash
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

- [ ] **Step 2: Run ASAN test suite**
```bash
cmake --build build_asan -j$(nproc)
ctest --test-dir build_asan -E test_integration --timeout 10 --output-on-failure
```

- [ ] **Step 3: Run clang-format**
```bash
cmake --build build --target format
cmake --build build --target check-format
```

- [ ] **Step 4: Final commit**
```bash
git add -A
git commit -m "refactor(h2): ♻️ optimize HTTP/2 stream allocation with per-connection pooling"
```
