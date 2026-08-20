# Arena Fast-Path Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `src/core/primitives/arena.c` so that the arena allocation fast-path operates with minimal loads, stores, and branches using cached `arena->ptr` and `arena->end` pointers directly in `csilk_arena_t`.

---

### Task 1: Update `csilk_arena_s` Struct Layout and Fast-Path Implementation

**Files:**
- Modify: `src/core/primitives/arena.c`

- [x] **Step 1: Update `csilk_arena_s` struct definition**
  Include `uint8_t* ptr`, `uint8_t* end`, `csilk_arena_chunk_t* head`, `size_t default_chunk_size`, `size_t max_total_bytes`, `size_t total_allocated`, `int align_64`, with padding to 64 bytes.

- [x] **Step 2: Implement `arena_alloc_slow()` and `csilk_arena_alloc()` fast-path**
  Ensure 8-byte fast path has minimal branches and cache references.

- [x] **Step 3: Update `csilk_arena_new()`, `csilk_arena_reset()`, `csilk_arena_free()`, `csilk_arena_get_stats()`**
  Keep `arena->ptr` and `arena->end` in sync with active chunk.

- [x] **Step 4: Verify existing arena tests pass**
  Run `ctest --test-dir build -R test_arena`

- [x] **Step 5: Commit Task 1**
```bash
git add src/core/primitives/arena.c
git commit -m "feat(arena): ✨ optimize allocation fast path with cached ptr/end pointers"
```

---

### Task 2: Implement Comprehensive Benchmark (8/32/128/1024 bytes & cycles/alloc)

**Files:**
- Create: `tests/core/test_arena_bench.c`
- Modify: `cmake/tests.cmake`

- [x] **Step 1: Write `tests/core/test_arena_bench.c`**
  Benchmark 8B, 32B, 128B, 1024B allocations with `__rdtsc()` cycles measurement.

- [x] **Step 2: Register in `cmake/tests.cmake`**

- [x] **Step 3: Build and run benchmark**

- [x] **Step 4: Commit Task 2**
```bash
git add tests/core/test_arena_bench.c cmake/tests.cmake
git commit -m "test(arena): ✅ add arena allocation fast-path latency & cycles benchmark"
```

---

### Task 3: Full Verification (Unit Tests, ASAN, TSAN, Formatting)

- [x] **Step 1: Run full unit test suite via `ctest`**
- [x] **Step 2: Run TSAN tests**
- [x] **Step 3: Run code formatting**
- [x] **Step 4: Final commit**
