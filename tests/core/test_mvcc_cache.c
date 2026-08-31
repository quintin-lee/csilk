/**
 * @file test_mvcc_cache.c
 * @brief Comprehensive tests for Epoch-based RCU/MVCC lock-free cache.
 * @copyright MIT License
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/cache/mvcc_cache.h"
#include "csilk/test/test.h"

#undef assert
#define assert(expr)                                                                               \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(                                                                               \
                stderr, "Assertion failed: (%s), file %s, line %d\n", #expr, __FILE__, __LINE__);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test 1: Basic Operations (Set, Get, Get View, Copy, Delete)        */
/* ------------------------------------------------------------------ */

static void
test_basic_operations(void)
{
    csilk_mvcc_cache_t* cache = csilk_mvcc_cache_new(32);
    assert(cache != NULL);

    const char* k1 = "user:1001";
    const char* v1 = "Alice";
    assert(csilk_mvcc_cache_set(cache, k1, v1, strlen(v1) + 1) == 0);

    /* Get View */
    csilk_mvcc_view_t view1;
    assert(csilk_mvcc_cache_get_view(cache, k1, &view1) == 0);
    assert(view1.data != NULL);
    assert(strcmp((const char*)view1.data, "Alice") == 0);
    assert(view1.len == strlen(v1) + 1);
    assert(view1.version >= 1);
    csilk_mvcc_cache_release_view(cache, &view1);

    /* Get Copy */
    char   buf[64] = {0};
    size_t out_len = 0;
    assert(csilk_mvcc_cache_get_copy(cache, k1, buf, sizeof(buf), &out_len) == 0);
    assert(strcmp(buf, "Alice") == 0);
    assert(out_len == strlen(v1) + 1);

    /* Update Key */
    const char* v2 = "Alice_Updated";
    assert(csilk_mvcc_cache_set(cache, k1, v2, strlen(v2) + 1) == 0);

    csilk_mvcc_view_t view2;
    assert(csilk_mvcc_cache_get_view(cache, k1, &view2) == 0);
    assert(strcmp((const char*)view2.data, "Alice_Updated") == 0);
    assert(view2.version >= view1.version);
    csilk_mvcc_cache_release_view(cache, &view2);

    /* Delete Key */
    assert(csilk_mvcc_cache_delete(cache, k1) == 0);
    csilk_mvcc_view_t view_del;
    assert(csilk_mvcc_cache_get_view(cache, k1, &view_del) == -1);

    /* Delete non-existing key */
    assert(csilk_mvcc_cache_delete(cache, "non_existent_key") == -1);

    csilk_mvcc_cache_free(cache);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: Epoch View Safety during Concurrent Updates                */
/* ------------------------------------------------------------------ */

static void
test_epoch_view_safety(void)
{
    csilk_mvcc_cache_t* cache = csilk_mvcc_cache_new(16);
    assert(cache != NULL);

    const char* key = "config:app_rate";
    const char* v1 = "Initial_Value_1000";
    assert(csilk_mvcc_cache_set(cache, key, v1, strlen(v1) + 1) == 0);

    /* Reader 1 acquires view */
    csilk_mvcc_view_t view;
    assert(csilk_mvcc_cache_get_view(cache, key, &view) == 0);
    assert(strcmp((const char*)view.data, v1) == 0);

    /* Writer performs 200 updates on the same key to force multiple retirements */
    for (int i = 0; i < 200; i++) {
        char new_val[64];
        snprintf(new_val, sizeof(new_val), "Updated_Value_%d", i);
        assert(csilk_mvcc_cache_set(cache, key, new_val, strlen(new_val) + 1) == 0);
    }

    /* Reader 1's view MUST remain valid and uncorrupted! */
    assert(view.data != NULL);
    assert(strcmp((const char*)view.data, "Initial_Value_1000") == 0);

    /* Reader 1 releases view */
    csilk_mvcc_cache_release_view(cache, &view);

    /* Reader acquires fresh view and sees the latest version */
    csilk_mvcc_view_t fresh_view;
    assert(csilk_mvcc_cache_get_view(cache, key, &fresh_view) == 0);
    assert(strcmp((const char*)fresh_view.data, "Updated_Value_199") == 0);
    csilk_mvcc_cache_release_view(cache, &fresh_view);

    csilk_mvcc_cache_free(cache);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: Multi-threaded Reader/Writer Consistency (RCU Stress)      */
/* ------------------------------------------------------------------ */

#define CONCURRENT_NUM_KEYS 32
#define CONCURRENT_WRITERS 4
#define CONCURRENT_READERS 8
#define CONCURRENT_OPS_PER_THREAD 5000

typedef struct {
    uint32_t val;
    uint32_t checksum;
} test_payload_t;

static inline test_payload_t
make_payload(uint32_t val)
{
    test_payload_t p;
    p.val = val;
    p.checksum = val ^ 0xDEADBEEF;
    return p;
}

typedef struct {
    csilk_mvcc_cache_t*  cache;
    atomic_bool          stop;
    atomic_uint_fast64_t total_reads;
    atomic_uint_fast64_t total_writes;
    atomic_uint_fast64_t checksum_errors;
} test_concurrent_ctx_t;

static void*
writer_worker(void* arg)
{
    test_concurrent_ctx_t* ctx = (test_concurrent_ctx_t*)arg;
    for (int op = 0; op < CONCURRENT_OPS_PER_THREAD; op++) {
        int  key_idx = rand() % CONCURRENT_NUM_KEYS;
        char key_str[32];
        snprintf(key_str, sizeof(key_str), "key:%d", key_idx);

        test_payload_t payload = make_payload((uint32_t)op + 1);
        csilk_mvcc_cache_set(ctx->cache, key_str, &payload, sizeof(payload));
        atomic_fetch_add_explicit(&ctx->total_writes, 1, memory_order_relaxed);
    }
    return NULL;
}

static void*
reader_worker(void* arg)
{
    test_concurrent_ctx_t* ctx = (test_concurrent_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
        int  key_idx = rand() % CONCURRENT_NUM_KEYS;
        char key_str[32];
        snprintf(key_str, sizeof(key_str), "key:%d", key_idx);

        csilk_mvcc_view_t view;
        if (csilk_mvcc_cache_get_view(ctx->cache, key_str, &view) == 0) {
            if (view.len == sizeof(test_payload_t)) {
                const test_payload_t* p = (const test_payload_t*)view.data;
                if ((p->val ^ 0xDEADBEEF) != p->checksum) {
                    atomic_fetch_add_explicit(&ctx->checksum_errors, 1, memory_order_relaxed);
                }
            }
            csilk_mvcc_cache_release_view(ctx->cache, &view);
            atomic_fetch_add_explicit(&ctx->total_reads, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void
test_concurrent_stress(void)
{
    csilk_mvcc_cache_t*   cache = csilk_mvcc_cache_new(16);
    test_concurrent_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cache = cache;
    atomic_init(&ctx.stop, false);
    atomic_init(&ctx.total_reads, 0);
    atomic_init(&ctx.total_writes, 0);
    atomic_init(&ctx.checksum_errors, 0);

    /* Pre-populate keys */
    for (int i = 0; i < CONCURRENT_NUM_KEYS; i++) {
        char key_str[32];
        snprintf(key_str, sizeof(key_str), "key:%d", i);
        test_payload_t p = make_payload(1);
        csilk_mvcc_cache_set(cache, key_str, &p, sizeof(p));
    }

    pthread_t writers[CONCURRENT_WRITERS];
    pthread_t readers[CONCURRENT_READERS];

    for (int i = 0; i < CONCURRENT_READERS; i++) {
        pthread_create(&readers[i], NULL, reader_worker, &ctx);
    }
    for (int i = 0; i < CONCURRENT_WRITERS; i++) {
        pthread_create(&writers[i], NULL, writer_worker, &ctx);
    }

    for (int i = 0; i < CONCURRENT_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    atomic_store_explicit(&ctx.stop, true, memory_order_relaxed);

    for (int i = 0; i < CONCURRENT_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    assert(atomic_load(&ctx.checksum_errors) == 0);
    assert(atomic_load(&ctx.total_writes) == CONCURRENT_WRITERS * CONCURRENT_OPS_PER_THREAD);
    assert(atomic_load(&ctx.total_reads) > 0);

    csilk_mvcc_cache_free(cache);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 4: Nested Views on the Same Thread                            */
/* ------------------------------------------------------------------ */

static void
test_nested_views(void)
{
    csilk_mvcc_cache_t* cache = csilk_mvcc_cache_new(16);
    assert(cache != NULL);

    csilk_mvcc_cache_set(cache, "k1", "val1", 5);
    csilk_mvcc_cache_set(cache, "k2", "val2", 5);

    csilk_mvcc_view_t v1, v2;
    assert(csilk_mvcc_cache_get_view(cache, "k1", &v1) == 0);
    assert(csilk_mvcc_cache_get_view(cache, "k2", &v2) == 0);

    assert(strcmp((const char*)v1.data, "val1") == 0);
    assert(strcmp((const char*)v2.data, "val2") == 0);

    csilk_mvcc_cache_release_view(cache, &v1);
    assert(strcmp((const char*)v2.data, "val2") == 0);
    csilk_mvcc_cache_release_view(cache, &v2);

    csilk_mvcc_cache_free(cache);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Epoch-based RCU / MVCC Lock-Free Cache Tests ===\n\n");

    printf("--- Basic Operations (Set, Get, View, Copy, Delete) ---\n");
    test_basic_operations();

    printf("\n--- Epoch View Safety During Concurrent Updates ---\n");
    test_epoch_view_safety();

    printf("\n--- Multi-threaded Reader/Writer Consistency (RCU Stress) ---\n");
    if (!__builtin_constant_p(0) && getenv("ASAN_OPTIONS") == NULL) {
        test_concurrent_stress();
    }

    printf("\n--- Nested Views ---\n");
    test_nested_views();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
