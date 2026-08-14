# Arena TLS Free List Chunk Reuse Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all identified bugs and architectural defects in the arena allocator's TLS chunk free list reuse mechanism, including quota bypass, pthread key destructor leak, multi-tier caching, uninitialized memory semantics, and documentation alignment, across four phased commits.

**Architecture:** 
1. Quota Enforcement: Unify `max_total_bytes` checks before acquiring any chunk (TLS or OS allocation) and fix `default_chunk_size == 0` fallback.
2. Multi-Tier TLS Cache: Replace single-linked-list 4KB cache with multi-tier cache (4KB, 16KB, 64KB) matching `srv_internal.h` definitions.
3. Thread Lifecycle & Cleanup: Ensure `pthread_setspecific` is invoked so POSIX thread-exit destructors run automatically, plus add defense-in-depth flushes on worker threads.
4. Memory Semantics & Documentation: Add `csilk_arena_calloc`, clarify `csilk_arena_alloc` uninitialized contract in public headers, and update formal verification documentation.

**Tech Stack:** C23, POSIX pthreads, CMake, Clang, CTest, ASAN.

---

### Task 1: Enforce `max_total_bytes` on TLS Reuse & Fix `default_chunk_size == 0` Fallback

**Files:**
- Modify: `src/core/primitives/arena.c`
- Modify: `tests/core/test_arena.c`

- [ ] **Step 1: Write failing test in `tests/core/test_arena.c`**

Add tests for:
1. `test_arena_max_total_bytes_tls_enforced()`: Verify that when `max_total_bytes` is set (e.g., 2048 bytes), `csilk_arena_alloc` returns `NULL` even when a 4096-byte chunk is available in the TLS free list.
2. `test_arena_default_chunk_size_zero()`: Verify that `csilk_arena_new(0)` sets default chunk size to `CSILK_DEFAULT_ARENA_SIZE` (4096), so allocations populate and reuse the TLS cache.

```c
void test_arena_max_total_bytes_tls_enforced(void) {
    printf("Testing csilk_arena max_total_bytes with TLS cache hit...\n");

    /* Populate TLS cache with a standard chunk */
    csilk_arena_t* a1 = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    assert(a1 != NULL);
    void* p = csilk_arena_alloc(a1, 100);
    assert(p != NULL);
    csilk_arena_free(a1);

    /* Allocate a new arena with a tight size limit smaller than 4KB */
    csilk_arena_t* a2 = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    assert(a2 != NULL);
    assert(csilk_arena_set_max_bytes(a2, 2048) == 0);

    /* Should fail because 4096 > 2048, even though TLS has a cached chunk */
    void* p2 = csilk_arena_alloc(a2, 100);
    assert(p2 == NULL);

    csilk_arena_free(a2);
    printf("csilk_arena max_total_bytes TLS enforcement passed!\n");
}

void test_arena_default_chunk_size_zero(void) {
    printf("Testing csilk_arena_new(0) defaults to CSILK_DEFAULT_ARENA_SIZE...\n");

    csilk_arena_t* a = csilk_arena_new(0);
    assert(a != NULL);
    void* p = csilk_arena_alloc(a, 64);
    assert(p != NULL);
    
    size_t total_size = 0, total_used = 0;
    csilk_arena_get_stats(a, &total_size, &total_used);
    assert(total_size == CSILK_DEFAULT_ARENA_SIZE);

    csilk_arena_free(a);
    printf("csilk_arena_new(0) default chunk size passed!\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_arena --output-on-failure`  
Expected: FAIL (assertion in `test_arena_max_total_bytes_tls_enforced` fails because `p2 != NULL`).

- [ ] **Step 3: Fix `max_total_bytes` quota check and default chunk size in `src/core/primitives/arena.c`**

1. In `csilk_arena_new`:
```c
csilk_arena_t*
csilk_arena_new(size_t default_chunk_size)
{
    csilk_arena_t* arena = arena_aligned_alloc(sizeof(csilk_arena_t));
    if (!arena) {
        return NULL;
    }
    arena->head = NULL;
    arena->default_chunk_size =
        default_chunk_size > 0 ? default_chunk_size : CSILK_DEFAULT_ARENA_SIZE;
    arena->align_64 = 0;
    arena->max_total_bytes = 0; /* Unlimited by default */
    arena->total_allocated = 0; /* Reset counter on creation */
    return arena;
}
```

2. In `csilk_arena_alloc`:
Move the `max_total_bytes` and integer overflow guards before TLS pop or allocation:
```c
    size_t chunk_size = size > arena->default_chunk_size ? size : arena->default_chunk_size;
    csilk_arena_chunk_t* chunk = NULL;

    /* Guard sizeof(chunk) + chunk_size against integer overflow */
    if (chunk_size > SIZE_MAX - sizeof(csilk_arena_chunk_t)) {
        return NULL;
    }

    /* Check if allocation would exceed max_total_bytes limit */
    if (arena->max_total_bytes > 0 &&
        (arena->total_allocated + chunk_size > arena->max_total_bytes)) {
        return NULL;
    }

    /* Try to reuse a chunk from the thread-local free list if it matches the
     standard size. This avoids expensive aligned_alloc syscalls in the
     hot path. */
    if (chunk_size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_free_list) {
        chunk = tls_chunk_free_list;
        tls_chunk_free_list = chunk->next;
        tls_chunk_count--;
    } else {
        chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R test_arena --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Apply code formatting and commit**

Run:
```bash
cmake --build build --target format
git add src/core/primitives/arena.c tests/core/test_arena.c
git commit -m "fix(arena): 🐛 enforce max_total_bytes on tls reuse and default chunk size"
```

---

### Task 2: Implement Multi-Tier TLS Chunk Cache (4KB, 16KB, 64KB)

**Files:**
- Modify: `src/core/primitives/arena.c`
- Modify: `tests/core/test_arena.c`

- [ ] **Step 1: Write failing test in `tests/core/test_arena.c`**

Add `test_arena_multi_tier_tls_cache()` testing 4KB, 16KB, and 64KB caching and reuse:

```c
void test_arena_multi_tier_tls_cache(void) {
    printf("Testing multi-tier TLS chunk caching (4K, 16K, 64K)...\n");

    /* Flush cache initially */
    csilk_arena_flush_free_list();

    /* Test 16KB tier */
    csilk_arena_t* a16 = csilk_arena_new(16384);
    assert(a16 != NULL);
    csilk_arena_alloc(a16, 5000); /* Will allocate a 16KB chunk */
    csilk_arena_free(a16);        /* Returns 16KB chunk to tier 1 */

    csilk_arena_t* a16_2 = csilk_arena_new(16384);
    void* p16 = csilk_arena_alloc(a16_2, 5000);
    assert(p16 != NULL);
    csilk_arena_free(a16_2);

    /* Test 64KB tier */
    csilk_arena_t* a64 = csilk_arena_new(65536);
    assert(a64 != NULL);
    csilk_arena_alloc(a64, 30000); /* Will allocate a 64KB chunk */
    csilk_arena_free(a64);         /* Returns 64KB chunk to tier 2 */

    csilk_arena_t* a64_2 = csilk_arena_new(65536);
    void* p64 = csilk_arena_alloc(a64_2, 30000);
    assert(p64 != NULL);
    csilk_arena_free(a64_2);

    csilk_arena_flush_free_list();
    printf("multi-tier TLS chunk caching passed!\n");
}
```

- [ ] **Step 2: Run test to verify it compiles and check behavior**

Run: `ctest --test-dir build -R test_arena --output-on-failure`

- [ ] **Step 3: Implement multi-tier caching in `src/core/primitives/arena.c`**

Replace single `tls_chunk_free_list` with tier array:
```c
static inline int
arena_size_to_tier(size_t size)
{
    if (size == CSILK_DEFAULT_ARENA_SIZE) {
        return CSILK_ARENA_TIER_SMALL; /* 4KB */
    }
    if (size == 16384) {
        return CSILK_ARENA_TIER_MEDIUM; /* 16KB */
    }
    if (size == 65536) {
        return CSILK_ARENA_TIER_LARGE; /* 64KB */
    }
    return -1;
}

static _Thread_local csilk_arena_chunk_t* tls_tier_free_lists[CSILK_ARENA_TIER_COUNT] = {NULL};
static _Thread_local int                  tls_tier_counts[CSILK_ARENA_TIER_COUNT] = {0};
```

Update `csilk_arena_alloc`:
```c
    int tier = arena_size_to_tier(chunk_size);
    if (tier >= 0 && tls_tier_free_lists[tier]) {
        chunk = tls_tier_free_lists[tier];
        tls_tier_free_lists[tier] = chunk->next;
        tls_tier_counts[tier]--;
    } else {
        chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
    }
```

Update `csilk_arena_free`:
```c
    int tier = arena_size_to_tier(curr->size);
    if (tier >= 0 && tls_tier_counts[tier] < MAX_TLS_CHUNKS_PER_TIER) {
        curr->next = tls_tier_free_lists[tier];
        curr->used = 0;
        tls_tier_free_lists[tier] = curr;
        tls_tier_counts[tier]++;
    } else {
        arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
    }
```

Update `csilk_arena_reset`:
```c
    int tier = arena_size_to_tier(curr->size);
    if (tier >= 0 && tls_tier_counts[tier] < MAX_TLS_CHUNKS_PER_TIER) {
        curr->next = tls_tier_free_lists[tier];
        curr->used = 0;
        tls_tier_free_lists[tier] = curr;
        tls_tier_counts[tier]++;
    } else {
        arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
    }
```

Update `csilk_arena_flush_free_list`:
```c
void
csilk_arena_flush_free_list(void)
{
    for (int t = 0; t < CSILK_ARENA_TIER_COUNT; t++) {
        csilk_arena_chunk_t* curr = tls_tier_free_lists[t];
        while (curr) {
            csilk_arena_chunk_t* next = curr->next;
            arena_aligned_free(curr, curr->size + sizeof(csilk_arena_chunk_t));
            curr = next;
        }
        tls_tier_free_lists[t] = NULL;
        tls_tier_counts[t] = 0;
    }
}
```

Update `csilk_arena_get_tls_chunk_count` for tests to return total cached count across all tiers.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R test_arena --output-on-failure`  
Expected: PASS.

- [ ] **Step 5: Apply code formatting and commit**

Run:
```bash
cmake --build build --target format
git add src/core/primitives/arena.c tests/core/test_arena.c
git commit -m "feat(arena): ✨ implement multi-tier chunk tls caching for 4k 16k and 64k"
```

---

### Task 3: Fix POSIX `pthread_key` Destructor Association & Worker Exit Cleanup

**Files:**
- Modify: `src/core/primitives/arena.c`
- Modify: `src/core/server/server_worker.c`
- Modify: `src/core/uring/uring_event_loop.c`
- Test: `tests/core/test_arena.c`

- [ ] **Step 1: Write multi-threaded thread-exit test in `tests/core/test_arena.c`**

Add a test that spawns a thread, uses arenas (populating the TLS cache), exits without manual flush, and verifies via pthread key destructor that `csilk_arena_flush_free_list()` was triggered:

```c
#include <pthread.h>

static void* thread_alloc_and_exit(void* arg) {
    (void)arg;
    csilk_arena_t* a = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    csilk_arena_alloc(a, 100);
    csilk_arena_free(a); /* Pushes chunk to thread TLS */
    return NULL;
}

void test_arena_pthread_tls_cleanup(void) {
    printf("Testing pthread key destructor on thread exit...\n");
    pthread_t tid;
    assert(pthread_create(&tid, NULL, thread_alloc_and_exit, NULL) == 0);
    assert(pthread_join(tid, NULL) == 0);
    printf("pthread key destructor passed!\n");
}
```

- [ ] **Step 2: Run test to verify behavior**

Run: `ctest --test-dir build -R test_arena --output-on-failure`

- [ ] **Step 3: Associate `pthread_key` with non-NULL value and add worker exit flush**

1. In `src/core/primitives/arena.c`:
Make `tls_key` visible within file and register specific non-null value on first cache usage:
```c
static pthread_key_t g_arena_tls_key;

static void
arena_tls_cleanup(void* val)
{
    (void)val;
    csilk_arena_flush_free_list();
}

static void
arena_init_tls_key(void)
{
    pthread_key_create(&g_arena_tls_key, arena_tls_cleanup);
}

void
csilk_arena_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, arena_init_tls_key);
}

static inline void
arena_ensure_tls_cleanup_registered(void)
{
    csilk_arena_init();
    if (pthread_getspecific(g_arena_tls_key) == NULL) {
        pthread_setspecific(g_arena_tls_key, (void*)1);
    }
}
```
Call `arena_ensure_tls_cleanup_registered()` when pushing chunks into `tls_tier_free_lists` in `csilk_arena_free` and `csilk_arena_reset`.

2. In `src/core/server/server_worker.c`:
At the end of `worker_thread()`:
```c
    csilk_io_run(loop_ptr, CSILK_IO_RUN_DEFAULT);
    csilk_arena_flush_free_list();
    uv_loop_close(loop_ptr);
```

3. In `src/core/uring/uring_event_loop.c`:
At the end of `uring_worker_thread()`:
```c
    csilk_arena_flush_free_list();
    io_uring_queue_exit(loop_ptr);
    return NULL;
```

- [ ] **Step 4: Run full test suite and verify no leaks or crashes**

Run: `ctest --test-dir build -E test_integration --output-on-failure`  
Expected: 100% tests passed.

- [ ] **Step 5: Apply code formatting and commit**

Run:
```bash
cmake --build build --target format
git add src/core/primitives/arena.c src/core/server/server_worker.c src/core/uring/uring_event_loop.c tests/core/test_arena.c
git commit -m "fix(arena): 🐛 ensure pthread key destructor triggers and flush worker tls on exit"
```

---

### Task 4: Add `csilk_arena_calloc` & Update Header and Documentation Semantics

**Files:**
- Modify: `include/csilk/core/server.h`
- Modify: `src/core/primitives/arena.c`
- Modify: `docs/design/arena-formal-verification.md`
- Modify: `docs/zh-CN/module-design/arena.md`
- Test: `tests/core/test_arena.c`

- [ ] **Step 1: Write failing test in `tests/core/test_arena.c`**

Add `test_arena_calloc()`:
```c
void test_arena_calloc(void) {
    printf("Testing csilk_arena_calloc...\n");
    csilk_arena_t* a = csilk_arena_new(1024);
    assert(a != NULL);

    int* arr = (int*)csilk_arena_calloc(a, 10, sizeof(int));
    assert(arr != NULL);
    for (int i = 0; i < 10; i++) {
        assert(arr[i] == 0);
    }

    /* Overflow check */
    assert(csilk_arena_calloc(a, SIZE_MAX, 2) == NULL);

    csilk_arena_free(a);
    printf("csilk_arena_calloc passed!\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build -R test_arena --output-on-failure`  
Expected: Compilation failure (`csilk_arena_calloc` undeclared).

- [ ] **Step 3: Declare and implement `csilk_arena_calloc` and clarify docs**

1. In `include/csilk/core/server.h`:
Update `csilk_arena_alloc` docstring:
```c
/**
 * @brief Allocate memory from an arena (uninitialized bump allocation).
 *
 * The returned memory is valid until csilk_arena_free, csilk_arena_reset, or
 * csilk_ctx_cleanup. Note that memory returned is NOT guaranteed to be zeroed
 * when chunks are reused. Use csilk_arena_calloc() if zero-initialization is required.
 *
 * @param arena  The arena allocator.
 * @param size   Number of bytes to allocate.
 * @return Pointer to the allocated block, or NULL on allocation failure.
 */
void* csilk_arena_alloc(csilk_arena_t* arena, size_t size);

/**
 * @brief Allocate zero-initialised memory from an arena.
 *
 * Allocates @p count * @p size bytes and zeroes the allocated block.
 *
 * @param arena The arena allocator.
 * @param count Number of elements.
 * @param size  Size of each element.
 * @return Pointer to zeroed block, or NULL on allocation failure / overflow.
 */
void* csilk_arena_calloc(csilk_arena_t* arena, size_t count, size_t size);
```

2. In `src/core/primitives/arena.c`:
Implement `csilk_arena_calloc`:
```c
void*
csilk_arena_calloc(csilk_arena_t* arena, size_t count, size_t size)
{
    if (count > 0 && size > SIZE_MAX / count) {
        return NULL;
    }
    size_t total = count * size;
    void*  ptr = csilk_arena_alloc(arena, total);
    if (ptr && total > 0) {
        memset(ptr, 0, total);
    }
    return ptr;
}
```

3. In `docs/design/arena-formal-verification.md` and `docs/zh-CN/module-design/arena.md`:
Update `csilk_arena_reset()` description to explicitly document the head-retention and tail-ejection-to-TLS behavior.

- [ ] **Step 4: Run all unit tests and verify clean pass**

Run:
```bash
ctest --test-dir build -E test_integration --output-on-failure
```
Expected: 100% passed.

- [ ] **Step 5: Apply code formatting and commit**

Run:
```bash
cmake --build build --target format
git add include/csilk/core/server.h src/core/primitives/arena.c docs/ tests/core/test_arena.c
git commit -m "docs(arena): 📝 align header docs with uninitialized alloc semantics and add arena calloc"
```
