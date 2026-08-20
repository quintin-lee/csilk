# Async Lock-Free Structured Logger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `src/core/config/logger.c` to an asynchronous, lock-free, zero-allocation logging architecture with dedicated background writer thread, zero-overhead disabled level filtering, configurable overflow strategies, file rotation, and shutdown flushing.

**Architecture:** Lock-free preallocated node pool + intrusive wait-free MPSC queue (`csilk_lfqueue_t`) + dedicated background consumer thread writing to `FILE*`. Producer threads format directly into node buffer without global lock or malloc. Atomic level check macro avoids any function or formatting overhead for disabled levels.

**Tech Stack:** C23, pthread / csilk_thread, C11 stdatomic, Clang/GCC, AddressSanitizer, ThreadSanitizer, CTest, CMake.

---

### Task 1: Update Logger Config & Macro Definitions in Public Headers

**Files:**
- Modify: `include/csilk/core/types.h`
- Modify: `include/csilk/core/server.h`

- [ ] **Step 1: Add `csilk_log_overflow_t` and `queue_capacity` to `csilk_log_config_t` in `include/csilk/core/types.h`**
```c
typedef enum {
    CSILK_LOG_OVERFLOW_DROP = 0,     /**< Drop new messages when queue is full. */
    CSILK_LOG_OVERFLOW_BLOCK = 1,    /**< Block/yield until space is available. */
    CSILK_LOG_OVERFLOW_FALLBACK = 2  /**< Write synchronously to stderr on overflow. */
} csilk_log_overflow_t;
```

- [ ] **Step 2: Update `CSILK_LOG_*` macros with atomic level filter in `include/csilk/core/server.h`**
Declare `g_csilk_log_level` and `g_csilk_log_initialized` and define `CSILK_LOG_IS_ENABLED(lv)`.

- [ ] **Step 3: Commit Task 1**
```bash
git add include/csilk/core/types.h include/csilk/core/server.h
git commit -m "feat(logger): ✨ add overflow strategies and zero-overhead macro filtering"
```

---

### Task 2: Implement Async Lock-Free Logger in `src/core/config/logger.c`

**Files:**
- Modify: `src/core/config/logger.c`

- [ ] **Step 1: Implement lock-free node pool and MPSC queue structures**
Define `csilk_log_node_t`, node pool freelist (Treiber stack with ABA protection / preallocated array), and logger state.

- [ ] **Step 2: Implement background logger thread**
Loop with batch dequeue, file size tracking, file rotation, periodic `fflush()`, and condvar sleep/wakeup.

- [ ] **Step 3: Implement producer formatting and queue push with overflow policies**
Implement `_csilk_log_internal` and `_csilk_log_structured` with drop/block/fallback support.

- [ ] **Step 4: Implement `csilk_log_flush()` and clean `csilk_log_close()`**
Draining queue, stopping thread, closing file, freeing node slab.

- [ ] **Step 5: Commit Task 2**
```bash
git add src/core/config/logger.c
git commit -m "feat(logger): ✨ implement asynchronous lock-free MPSC logger"
```

---

### Task 3: Add Async Logger Benchmarks & Correctness Tests

**Files:**
- Create: `tests/core/test_logger_async_bench.c`
- Modify: `cmake/tests.cmake`

- [ ] **Step 1: Write `tests/core/test_logger_async_bench.c`**
Implement tests covering:
1. Multi-threaded logging correctness (text and JSON mode, request ID tracking).
2. Overflow strategies: `DROP`, `BLOCK`, `FALLBACK`.
3. File rotation under concurrent async logging.
4. Benchmark across 0, 1, 10, 100 logs/request with multi-worker threads (1, 4, 8 threads).
5. Compare async throughput vs synchronous mutex logging.

- [ ] **Step 2: Register in `cmake/tests.cmake`**
Add `test_logger_async_bench` to `CSILK_CORE_TESTS` and `CSILK_CORE_TEST_DIRS`.

- [ ] **Step 3: Build and run test**
```bash
cmake -B build -S . && cmake --build build --target test_logger_async_bench
./build/test_logger_async_bench
```

- [ ] **Step 4: Commit Task 3**
```bash
git add tests/core/test_logger_async_bench.c cmake/tests.cmake
git commit -m "test(logger): ✅ add async logger correctness tests and 0/1/10/100 logs/req benchmarks"
```

---

### Task 4: Full Test Suite, ASAN/TSAN Verification & Formatting

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

- [ ] **Step 3: Run TSAN test suite**
```bash
cmake -B build_tsan -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DUSE_TSAN=ON -DENABLE_OOM_TEST=ON
cmake --build build_tsan -j$(nproc)
ctest --test-dir build_tsan -R test_logger --timeout 10 --output-on-failure
```

- [ ] **Step 4: Code formatting**
```bash
cmake --build build --target format
cmake --build build --target check-format
```

- [ ] **Step 5: Final commit**
```bash
git add -A
git commit -m "refactor(logger): ♻️ complete async lock-free logger pipeline with full verification"
```
