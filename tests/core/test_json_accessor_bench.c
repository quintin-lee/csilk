/**
 * @file tests/core/test_json_accessor_bench.c
 * @brief Benchmark and stress test for JSON pointer accessors and view-lifetime safety.
 */

#include "csilk/core/json/json.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ====================================================================
 * Benchmark 1: Pointer Accessor Performance
 * ==================================================================== */

static void
bench_pointer_accessor(void)
{
    printf("=== JSON Benchmark: Pointer Accessors ===\n");

    const char* json_str = "{\"id\":12345,\"name\":\"csilk_framework\",\"active\":true,"
                           "\"score\":99.5,\"meta\":{\"cluster\":\"us-east-1\",\"nodes\":16}}";

    csilk_json_t* doc = csilk_json_parse(json_str);
    assert(doc != NULL);

    const int iterations = 200000;

    uint64_t start_ns = get_monotonic_ns();
    int64_t  p_sum = 0;
    for (int i = 0; i < iterations; i++) {
        csilk_json_t* id_ptr = csilk_json_get(doc, "id");
        p_sum += csilk_json_int_value(id_ptr);

        csilk_json_t* meta_ptr = csilk_json_get(doc, "meta");
        csilk_json_t* nodes_ptr = csilk_json_get(meta_ptr, "nodes");
        p_sum += csilk_json_int_value(nodes_ptr);
    }
    uint64_t p_dur_ns = get_monotonic_ns() - start_ns;
    double   p_ns_per_op = (double)p_dur_ns / (double)(iterations * 3);

    printf("  Pointer View  : %6.2f ns/lookup  (%8.1f M ops/sec)\n",
           p_ns_per_op,
           1000.0 / p_ns_per_op);

    csilk_json_free(doc);
}

/* ====================================================================
 * Test 2: Verify Borrowed Views Live Until Root Free
 * ==================================================================== */

static void
test_views_live_until_root_free(void)
{
    printf("=== Test: Array View Lifetime Safety ===\n");

    const int     total_items = 10000;
    csilk_json_t* arr = csilk_json_array();
    assert(arr != NULL);

    for (int i = 0; i < total_items; i++) {
        csilk_json_t* item = csilk_json_object();
        csilk_json_add_int(item, "id", i);
        csilk_json_array_append(arr, item);
    }

    size_t sz = csilk_json_array_size(arr);
    assert(sz == (size_t)total_items);

    csilk_json_t** saved_views = malloc(total_items * sizeof(*saved_views));
    assert(saved_views != NULL);

    for (int i = 0; i < total_items; i++) {
        saved_views[i] = csilk_json_array_get(arr, i);
        assert(saved_views[i] != NULL);
    }

    for (int i = 0; i < total_items; i++) {
        int64_t val = csilk_json_get_int(saved_views[i], "id");
        if (val != i) {
            fprintf(stderr,
                    "FATAL: View corruption detected at index %d: expected %d, got %" PRId64 "\n",
                    i,
                    i,
                    val);
            assert(val == i);
        }
    }

    free(saved_views);
    csilk_json_free(arr);
    printf("  [PASS] %d array views remained valid until root free.\n\n", total_items);
}

/* ====================================================================
 * Test 3: Cross-Thread & Async Read Safety (TSAN Validation)
 * ==================================================================== */

typedef struct {
    csilk_json_t* doc_view;
    int           thread_id;
    int           iterations;
} thread_arg_t;

static void*
reader_thread_fn(void* ptr)
{
    thread_arg_t* arg = (thread_arg_t*)ptr;
    for (int i = 0; i < arg->iterations; i++) {
        csilk_json_t* id_val = csilk_json_get(arg->doc_view, "task_id");
        int64_t       id = csilk_json_int_value(id_val);
        assert(id == 9999);

        const char* name = csilk_json_get_string(arg->doc_view, "service");
        assert(name != NULL && strcmp(name, "auth_service") == 0);
    }
    return NULL;
}

static void
test_cross_thread_view_safety(void)
{
    printf("=== Test: Concurrent Cross-Thread JSON View Reads (TSAN) ===\n");

    const char*   json_str = "{\"task_id\":9999,\"service\":\"auth_service\",\"threads\":8}";
    csilk_json_t* doc = csilk_json_parse(json_str);
    assert(doc != NULL);

    const int    num_threads = 8;
    pthread_t    threads[8];
    thread_arg_t args[8];

    for (int i = 0; i < num_threads; i++) {
        args[i].doc_view = doc;
        args[i].thread_id = i;
        args[i].iterations = 100000;
        pthread_create(&threads[i], NULL, reader_thread_fn, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    csilk_json_free(doc);
    printf(
        "  [PASS] 8 concurrent threads performed 800,000 cross-thread view lookups cleanly.\n\n");
}

int
main(void)
{
    printf("=================================================================\n");
    printf("           CSILK JSON POINTER ACCESSOR BENCHMARK & AUDIT        \n");
    printf("=================================================================\n\n");

    bench_pointer_accessor();
    test_views_live_until_root_free();
    test_cross_thread_view_safety();

    printf("=================================================================\n");
    printf("             ALL JSON BENCHMARK & AUDIT TESTS PASSED             \n");
    printf("=================================================================\n");
    return 0;
}
