#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "core/internal/srv_internal.h"
#include "core/http/h2.h"

/* Simple cycle counter for x86 / fallback */
#if defined(__x86_64__) || defined(_M_X64)
static inline uint64_t
rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static inline uint64_t
rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static inline double
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
/* Test 1: Basic CRUD Operations                                              */
/* -------------------------------------------------------------------------- */
static void
test_h2_stream_crud(void)
{
    printf("Testing HTTP/2 stream CRUD operations...\n");

    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    /* Create streams */
    csilk_ctx_t* s1 = csilk_h2_get_or_create_stream(&client, 1);
    csilk_ctx_t* s3 = csilk_h2_get_or_create_stream(&client, 3);
    csilk_ctx_t* s5 = csilk_h2_get_or_create_stream(&client, 5);
    csilk_ctx_t* s7 = csilk_h2_get_or_create_stream(&client, 7);

    assert(s1 != NULL && s1->stream_id == 1);
    assert(s3 != NULL && s3->stream_id == 3);
    assert(s5 != NULL && s5->stream_id == 5);
    assert(s7 != NULL && s7->stream_id == 7);
    assert(client.h2_stream_map.count == 4);

    /* Lookup existing streams */
    assert(csilk_h2_get_or_create_stream(&client, 1) == s1);
    assert(csilk_h2_get_or_create_stream(&client, 3) == s3);
    assert(csilk_h2_get_or_create_stream(&client, 5) == s5);
    assert(csilk_h2_get_or_create_stream(&client, 7) == s7);

    /* Remove stream 3 */
    int r = csilk_h2_remove_stream(&client, 3);
    assert(r == 0);
    assert(client.h2_stream_map.count == 3);

    /* Try removing stream 3 again (must fail with -1) */
    assert(csilk_h2_remove_stream(&client, 3) == -1);

    /* Try removing non-existent stream */
    assert(csilk_h2_remove_stream(&client, 999) == -1);

    /* Remaining streams intact */
    assert(csilk_h2_get_or_create_stream(&client, 1) == s1);
    assert(csilk_h2_get_or_create_stream(&client, 5) == s5);
    assert(csilk_h2_get_or_create_stream(&client, 7) == s7);

    /* Cleanup all remaining streams */
    csilk_h2_free_streams(&client);
    assert(client.h2_stream_map.count == 0);
    assert(client.h2_stream_map.buckets == client.h2_stream_map.inline_buckets);

    printf("test_h2_stream_crud: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Table Resizing and Collision Handling                              */
/* -------------------------------------------------------------------------- */
static void
test_h2_stream_resize_and_collision(void)
{
    printf("Testing HTTP/2 stream table dynamic resizing and collision handling...\n");

    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    const int    NUM_STREAMS = 250;
    csilk_ctx_t* ptrs[NUM_STREAMS];

    /* Insert 250 streams: 1, 3, 5, ..., 499 */
    for (int i = 0; i < NUM_STREAMS; i++) {
        int32_t stream_id = i * 2 + 1;
        ptrs[i] = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(ptrs[i] != NULL);
        assert(ptrs[i]->stream_id == stream_id);
    }

    assert(client.h2_stream_map.count == NUM_STREAMS);
    assert(client.h2_stream_map.capacity >= (uint32_t)NUM_STREAMS);
    assert(client.h2_stream_map.buckets != client.h2_stream_map.inline_buckets);

    /* Verify all can be found correctly */
    for (int i = 0; i < NUM_STREAMS; i++) {
        int32_t      stream_id = i * 2 + 1;
        csilk_ctx_t* found = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(found == ptrs[i]);
    }

    /* Delete even-indexed streams */
    for (int i = 0; i < NUM_STREAMS; i += 2) {
        int32_t stream_id = i * 2 + 1;
        assert(csilk_h2_remove_stream(&client, stream_id) == 0);
    }
    assert(client.h2_stream_map.count == NUM_STREAMS / 2);

    /* Verify odd-indexed streams still present */
    for (int i = 1; i < NUM_STREAMS; i += 2) {
        int32_t      stream_id = i * 2 + 1;
        csilk_ctx_t* found = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(found == ptrs[i]);
    }

    csilk_h2_free_streams(&client);
    assert(client.h2_stream_map.count == 0);
    assert(client.h2_stream_map.buckets == client.h2_stream_map.inline_buckets);

    printf("test_h2_stream_resize_and_collision: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Benchmark for 100, 1,000, 10,000 Concurrent Streams               */
/* -------------------------------------------------------------------------- */
static void
benchmark_stream_scale(int stream_count, int lookup_iterations)
{
    printf("\n=== Benchmarking %d Concurrent HTTP/2 Streams (%d lookups) ===\n",
           stream_count,
           lookup_iterations);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    int32_t* stream_ids = malloc(sizeof(int32_t) * (size_t)stream_count);
    assert(stream_ids != NULL);
    for (int i = 0; i < stream_count; i++) {
        stream_ids[i] = (int32_t)(i * 2 + 1);
    }

    /* 1. Insertion benchmark */
    double t_start = now_ns();
    for (int i = 0; i < stream_count; i++) {
        csilk_ctx_t* ctx = csilk_h2_get_or_create_stream(&client, stream_ids[i]);
        assert(ctx != NULL);
    }
    double t_insert = now_ns() - t_start;
    double ns_per_insert = t_insert / stream_count;

    printf("  Insert %d streams: %.2f ms (%.2f ns/insert, capacity: %u)\n",
           stream_count,
           t_insert / 1e6,
           ns_per_insert,
           client.h2_stream_map.capacity);

    /* 2. Lookup benchmark */
    uint64_t c_start = rdtsc();
    t_start = now_ns();

    uint32_t lfsr = 0xACE1u;
    for (int iter = 0; iter < lookup_iterations; iter++) {
        /* Pseudo-random stream index */
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        int idx = (int)(lfsr % (uint32_t)stream_count);

        csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, stream_ids[idx]);
        assert(c != NULL);
    }

    double   t_lookup = now_ns() - t_start;
    uint64_t c_elapsed = rdtsc() - c_start;

    double ns_per_lookup = t_lookup / lookup_iterations;
    double cycles_per_lookup = (double)c_elapsed / lookup_iterations;
    double lookups_per_sec = (double)lookup_iterations / (t_lookup / 1e9);

    printf("  Lookup benchmark:  %.2f ns/lookup | %.2f cycles/lookup | %.2f M ops/sec\n",
           ns_per_lookup,
           cycles_per_lookup,
           lookups_per_sec / 1e6);

    /* 3. Deletion benchmark */
    t_start = now_ns();
    for (int i = 0; i < stream_count; i++) {
        int r = csilk_h2_remove_stream(&client, stream_ids[i]);
        assert(r == 0);
    }
    double t_delete = now_ns() - t_start;
    double ns_per_delete = t_delete / stream_count;

    printf("  Delete %d streams: %.2f ms (%.2f ns/delete)\n",
           stream_count,
           t_delete / 1e6,
           ns_per_delete);

    assert(client.h2_stream_map.count == 0);
    csilk_h2_free_streams(&client);
    free(stream_ids);
}

/* -------------------------------------------------------------------------- */
/* Main Runner                                                                */
/* -------------------------------------------------------------------------- */
int
main(void)
{
    printf("=== Running HTTP/2 Stream Hash Map Tests & Benchmarks ===\n\n");

    test_h2_stream_crud();
    test_h2_stream_resize_and_collision();

    benchmark_stream_scale(100, 500000);
    benchmark_stream_scale(1000, 500000);
    benchmark_stream_scale(10000, 500000);

    printf("\n=== All HTTP/2 Stream Hash Map Tests Passed! ===\n");
    return 0;
}
