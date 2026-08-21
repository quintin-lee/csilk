#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS() printf("  [PASS] %s\n", __func__);

static void test_size_max_near_boundary(void) {
    printf("Running test_size_max_near_boundary...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    assert(csilk_arena_alloc(a, SIZE_MAX) == NULL);
    assert(csilk_arena_alloc(a, SIZE_MAX - 1) == NULL);
    assert(csilk_arena_alloc(a, SIZE_MAX - 7) == NULL);
    assert(csilk_arena_alloc(a, SIZE_MAX - 6) == NULL);
    csilk_arena_free(a); PASS();
}
static void test_max_total_bytes_enforced(void) {
    printf("Running test_max_total_bytes_enforced...\n");
    csilk_arena_t* a = csilk_arena_new(128); assert(a != NULL);
    assert(csilk_arena_set_max_bytes(a, 256) == 0);
    // 100 bytes leaves 28 remaining — next 100 forces new chunk
    void* p1 = csilk_arena_alloc(a, 100); assert(p1 != NULL);  // chunk1, total=128
    void* p2 = csilk_arena_alloc(a, 100); assert(p2 != NULL);  // chunk2, total=256
    void* p3 = csilk_arena_alloc(a, 100); assert(p3 == NULL);  // would be 384>256
    csilk_arena_free(a); PASS();
}
static void test_total_allocated_no_underflow(void) {
    printf("Running test_total_allocated_no_underflow...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    for (int i = 0; i < 10; i++) csilk_arena_alloc(a, 100);
    csilk_arena_reset(a); assert(csilk_arena_alloc(a, 1) != NULL);
    csilk_arena_free(a);
    a = csilk_arena_new(4096);
    void* p = csilk_arena_alloc(a, 64); assert(p != NULL);
    csilk_arena_free(a); PASS();
}
static void test_empty_arena_reset(void) {
    printf("Running test_empty_arena_reset...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    csilk_arena_reset(a); csilk_arena_reset(a); csilk_arena_reset(a);
    void* p = csilk_arena_alloc(a, 64); assert(p != NULL);
    memset(p, 0xAB, 64); csilk_arena_free(a); PASS();
}
static void test_null_arena(void) {
    printf("Running test_null_arena...\n");
    assert(csilk_arena_alloc(NULL, 64) == NULL);
    assert(csilk_arena_calloc(NULL, 10, 8) == NULL);
    assert(csilk_arena_strdup(NULL, "test") == NULL);
    assert(csilk_arena_strndup(NULL, "test", 4) == NULL);
    csilk_arena_free(NULL); csilk_arena_reset(NULL);
    csilk_arena_set_alignment(NULL, 1);
    assert(csilk_arena_set_max_bytes(NULL, 1024) == -1);
    size_t ts = 0, tu = 0; csilk_arena_get_stats(NULL, &ts, &tu);
    assert(csilk_arena_contains(NULL, (void*)0x1234) == 0); PASS();
}
static void test_get_stats_no_overflow(void) {
    printf("Running test_get_stats_no_overflow...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    for (int i = 0; i < 1000; i++) csilk_arena_alloc(a, 4000);
    size_t ts = 0, tu = 0; csilk_arena_get_stats(a, &ts, &tu);
    assert(ts > 0); assert(tu > 0); assert(tu <= ts);
    assert(ts <= (size_t)1000 * 4096); csilk_arena_free(a); PASS();
}
static void test_repeated_reset_tight_max_bytes(void) {
    printf("Running test_repeated_reset_tight_max_bytes...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    assert(csilk_arena_set_max_bytes(a, 4096) == 0);
    void* p1 = csilk_arena_alloc(a, 100); assert(p1 != NULL);
    csilk_arena_reset(a); void* p2 = csilk_arena_alloc(a, 100); assert(p2 != NULL);
    csilk_arena_free(a); PASS();
}
static void test_chunk_alignment_overflow_guard(void) {
    printf("Running test_chunk_alignment_overflow_guard...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    csilk_arena_set_alignment(a, 1);
    assert(csilk_arena_alloc(a, SIZE_MAX - 100) == NULL);
    void* p = csilk_arena_alloc(a, 64); assert(p != NULL);
    assert(((uintptr_t)p & 63) == 0); csilk_arena_free(a); PASS();
}
static void test_64_align_across_resets(void) {
    printf("Running test_64_align_across_resets...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    csilk_arena_set_alignment(a, 1);
    for (int r = 0; r < 10; r++) {
        for (int i = 0; i < 20; i++) {
            void* p = csilk_arena_alloc(a, 1); assert(p != NULL);
            assert(((uintptr_t)p & 63) == 0);
        } csilk_arena_reset(a);
    } csilk_arena_free(a); PASS();
}
static void test_fast_path_uintptr_safety(void) {
    printf("Running test_fast_path_uintptr_safety...\n");
    csilk_arena_t* a = csilk_arena_new(64); assert(a != NULL);
    for (int i = 0; i < 200; i++) {
        void* p = csilk_arena_alloc(a, 60); assert(p != NULL);
        assert(((uintptr_t)p & 7) == 0);
    }
    size_t ts = 0, tu = 0; csilk_arena_get_stats(a, &ts, &tu);
    assert(ts > 0); assert(tu > 0); csilk_arena_free(a); PASS();
}
static void test_total_allocated_tracking(void) {
    printf("Running test_total_allocated_tracking...\n");
    csilk_arena_t* a = csilk_arena_new(1024); assert(a != NULL);
    for (int i = 0; i < 5; i++) csilk_arena_alloc(a, 900);
    size_t ts = 0, tu = 0; csilk_arena_get_stats(a, &ts, &tu);
    assert(ts == 5 * 1024); csilk_arena_free(a); PASS();
}
static void test_tls_cache_respects_max_bytes(void) {
    printf("Running test_tls_cache_respects_max_bytes...\n");
    csilk_arena_t* a1 = csilk_arena_new(4096);
    csilk_arena_alloc(a1, 100); csilk_arena_free(a1);
    csilk_arena_t* a2 = csilk_arena_new(4096);
    csilk_arena_set_max_bytes(a2, 100);
    assert(csilk_arena_alloc(a2, 50) == NULL);
    csilk_arena_free(a2); PASS();
}
static void test_max_bytes_cumulative(void) {
    printf("Running test_max_bytes_cumulative...\n");
    csilk_arena_t* a = csilk_arena_new(1024); assert(a != NULL);
    assert(csilk_arena_set_max_bytes(a, 3072) == 0);
    // 900 bytes forces new chunk each time (1024-900=124 remaining)
    assert(csilk_arena_alloc(a, 900) != NULL);  // chunk1, total=1024
    assert(csilk_arena_alloc(a, 900) != NULL);  // chunk2, total=2048
    assert(csilk_arena_alloc(a, 900) != NULL);  // chunk3, total=3072
    assert(csilk_arena_alloc(a, 900) == NULL);  // chunk4 would be 4096>3072  // 4th chunk would make total=4096>3072
    csilk_arena_free(a); PASS();
}
static void test_free_underflow_guard(void) {
    printf("Running test_free_underflow_guard...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    csilk_arena_alloc(a, 100); csilk_arena_alloc(a, 100);
    csilk_arena_free(a);
    a = csilk_arena_new(4096);
    assert(csilk_arena_alloc(a, 100) != NULL);
    csilk_arena_free(a); PASS();
}
static void test_slow_path_many_chunks(void) {
    printf("Running test_slow_path_many_chunks...\n");
    csilk_arena_t* a = csilk_arena_new(64); assert(a != NULL);
    for (int i = 0; i < 100; i++) assert(csilk_arena_alloc(a, 60) != NULL);
    size_t ts = 0, tu = 0; csilk_arena_get_stats(a, &ts, &tu);
    assert(ts > 0); assert(tu > 0); assert(tu <= ts);
    csilk_arena_free(a); PASS();
}
static void test_calloc_overflow_guard(void) {
    printf("Running test_calloc_overflow_guard...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    assert(csilk_arena_calloc(a, SIZE_MAX, 2) == NULL);
    assert(csilk_arena_calloc(a, SIZE_MAX / 2 + 1, 3) == NULL);
    int* arr = (int*)csilk_arena_calloc(a, 10, sizeof(int));
    assert(arr != NULL);
    for (int i = 0; i < 10; i++) assert(arr[i] == 0);
    csilk_arena_free(a); PASS();
}
static void test_zero_size_alloc(void) {
    printf("Running test_zero_size_alloc...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    void* p = csilk_arena_alloc(a, 0); assert(p == (void*)(uintptr_t)1);
    p = csilk_arena_alloc(a, 64); assert(p != NULL);
    csilk_arena_free(a); PASS();
}
static void test_max_bytes_unlimited(void) {
    printf("Running test_max_bytes_unlimited...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    assert(csilk_arena_set_max_bytes(a, 0) == 0);
    for (int i = 0; i < 100; i++) assert(csilk_arena_alloc(a, 100) != NULL);
    csilk_arena_free(a); PASS();
}
static void test_reset_reuses_same_address(void) {
    printf("Running test_reset_reuses_same_address...\n");
    csilk_arena_t* a = csilk_arena_new(4096); assert(a != NULL);
    void* p1 = csilk_arena_alloc(a, 100); assert(p1 != NULL);
    csilk_arena_reset(a);
    void* p2 = csilk_arena_alloc(a, 100); assert(p2 == p1);
    csilk_arena_free(a); PASS();
}
static void test_custom_chunk_large_alignment(void) {
    printf("Running test_custom_chunk_large_alignment...\n");
    csilk_arena_t* a = csilk_arena_new(8192); assert(a != NULL);
    csilk_arena_set_alignment(a, 1);
    for (int i = 0; i < 50; i++) {
        void* p = csilk_arena_alloc(a, 1); assert(p != NULL);
        assert(((uintptr_t)p & 63) == 0);
    } csilk_arena_free(a); PASS();
}

int main(void) {
    printf("=== Arena UB Regression Test Suite ===\n\n");
    test_size_max_near_boundary();
    test_max_total_bytes_enforced();
    test_total_allocated_no_underflow();
    test_empty_arena_reset();
    test_null_arena();
    test_get_stats_no_overflow();
    test_repeated_reset_tight_max_bytes();
    test_chunk_alignment_overflow_guard();
    test_64_align_across_resets();
    test_fast_path_uintptr_safety();
    test_total_allocated_tracking();
    test_tls_cache_respects_max_bytes();
    test_max_bytes_cumulative();
    test_free_underflow_guard();
    test_slow_path_many_chunks();
    test_calloc_overflow_guard();
    test_zero_size_alloc();
    test_max_bytes_unlimited();
    test_reset_reuses_same_address();
    test_custom_chunk_large_alignment();
    printf("\n=== All arena UB regression tests passed! ===\n");
    return 0;
}
