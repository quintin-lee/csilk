# HTTP/2 Stream Hash Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor HTTP/2 stream management in `src/core/http/h2_session.c` and `src/core/http/h2_callbacks.c` from an $O(N)$ linked list to an adaptive power-of-two chained hash map supporting 100/1,000/10,000 concurrent streams with $O(1)$ lookup and deletion.

**Architecture:** Embed `csilk_h2_stream_map_t` with 16 inline buckets into `csilk_client_t` for zero-allocation fast-path. Adaptively expand bucket array on load. Reuse `ctx->next_stream` as collision chain pointer to avoid any node wrapper allocation. Ensure deterministic arena cleanup on stream closure.

**Tech Stack:** C23, nghttp2, Clang/GCC, AddressSanitizer, CTest, CMake.

---

### Task 1: Define `csilk_h2_stream_map_t` and Update `csilk_client_t`

**Files:**
- Modify: `src/core/internal/srv_internal.h`
- Modify: `src/core/server/connection_pool.c`

- [ ] **Step 1: Define `csilk_h2_stream_map_t` in `srv_internal.h`**
Add definition for `csilk_h2_stream_map_t` with `CSILK_H2_INLINE_BUCKETS 16`, and replace `csilk_ctx_t* h2_streams;` in `struct csilk_client_s` with `csilk_h2_stream_map_t h2_stream_map;`.

- [ ] **Step 2: Update connection reset and teardown in `connection_pool.c`**
Update `reset_hot_state` to properly reset `h2_stream_map` fields (clear counts, reset pointer to `inline_buckets`).

- [ ] **Step 3: Commit Task 1 changes**
```bash
git add src/core/internal/srv_internal.h src/core/server/connection_pool.c
git commit -m "feat(h2): ✨ define csilk_h2_stream_map_t with inline buckets in csilk_client_t"
```

---

### Task 2: Implement Adaptive Stream Hash Map in `h2_session.c` and `h2_callbacks.c`

**Files:**
- Modify: `src/core/http/h2.h`
- Modify: `src/core/http/h2_session.c`
- Modify: `src/core/http/h2_callbacks.c`

- [ ] **Step 1: Declare `csilk_h2_remove_stream` in `src/core/http/h2.h`**
```c
/**
 * @brief Remove and free a stream context from the client's stream map by stream ID.
 * @param client    The client connection.
 * @param stream_id The HTTP/2 stream ID.
 * @return 0 on success, -1 if not found.
 */
int csilk_h2_remove_stream(csilk_client_t* client, int32_t stream_id);
```

- [ ] **Step 2: Implement hash helper, lookup/create with auto-resizing, and stream map teardown in `h2_session.c`**
- `_csilk_h2_stream_hash(stream_id, mask)`
- `_csilk_h2_stream_map_resize(client)`
- `csilk_h2_get_or_create_stream(client, stream_id)`
- `csilk_h2_remove_stream(client, stream_id)`
- `csilk_h2_free_streams(client)`

- [ ] **Step 3: Update `on_stream_close_callback` in `h2_callbacks.c`**
Replace list search with `csilk_h2_remove_stream(client, stream_id);`.

- [ ] **Step 4: Build and test existing tests**
```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R test_h2 --timeout 10 --output-on-failure
```

- [ ] **Step 5: Commit Task 2 changes**
```bash
git add src/core/http/h2.h src/core/http/h2_session.c src/core/http/h2_callbacks.c
git commit -m "feat(h2): ✨ implement adaptive stream hash map for O(1) lookup and close"
```

---

### Task 3: Add High-Concurrency Stream Benchmark & Correctness Tests

**Files:**
- Create: `tests/core/test_h2_stream_bench.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: Write `tests/core/test_h2_stream_bench.c`**
Implement tests covering:
1. Basic get_or_create, lookup, duplicate lookup, stream close.
2. Collision distribution test with Knuth hash.
3. Resizing test across 16 -> 32 -> 64 -> 128 -> 256 -> 512 -> 1024 -> 2048 -> 4096 -> 8192 -> 16384.
4. Scale test with 100, 1,000, 10,000 concurrent streams:
   - Insertion time
   - Random lookup latency (cycles / op, ns / op)
   - Stream closure / removal time
5. Free all streams test.

- [ ] **Step 2: Register in `cmake/tests.cmake`**
Add `test_h2_stream_bench` to `CSILK_CORE_TESTS` and `CSILK_CORE_TEST_DIRS`.

- [ ] **Step 3: Build and run the benchmark**
```bash
cmake -B build -S . && cmake --build build --target test_h2_stream_bench
./build/test_h2_stream_bench
```

- [ ] **Step 4: Commit Task 3 changes**
```bash
git add tests/core/test_h2_stream_bench.c cmake/tests.cmake
git commit -m "test(h2): ✅ add stream hash map correctness and scale benchmarks for 100/1K/10K streams"
```

---

### Task 4: Full Test Suite, ASAN Verification & Clang-Format

**Files:**
- Modify (format): all touched files

- [ ] **Step 1: Run full unit test suite**
```bash
ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

- [ ] **Step 2: Run ASAN test suite**
```bash
cmake -B build_asan -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DUSE_ASAN=ON -DENABLE_OOM_TEST=ON -DCSILK_BUILD_SHARED=ON
cmake --build build_asan -j$(nproc)
ctest --test-dir build_asan -E test_integration --timeout 10 --output-on-failure
```

- [ ] **Step 3: Code formatting**
```bash
cmake --build build --target format
cmake --build build --target check-format
```

- [ ] **Step 4: Final commit & summary**
```bash
git add -A
git commit -m "refactor(h2): ♻️ optimize stream lookup and closure with adaptive hash map"
```
