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
    assert(client.h2_stream_map.pool_count == 1);

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
    assert(client.h2_stream_map.pool_count == 0);
    assert(client.h2_stream_map.free_list == NULL);
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

    /* Delete even-indexed streams (they will be returned to pool up to pool_max) */
    for (int i = 0; i < NUM_STREAMS; i += 2) {
        int32_t stream_id = i * 2 + 1;
        assert(csilk_h2_remove_stream(&client, stream_id) == 0);
    }
    assert(client.h2_stream_map.count == NUM_STREAMS / 2);
    assert(client.h2_stream_map.pool_count == CSILK_H2_STREAM_POOL_MAX);

    /* Verify odd-indexed streams still present */
    for (int i = 1; i < NUM_STREAMS; i += 2) {
        int32_t      stream_id = i * 2 + 1;
        csilk_ctx_t* found = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(found == ptrs[i]);
    }

    csilk_h2_free_streams(&client);
    assert(client.h2_stream_map.count == 0);
    assert(client.h2_stream_map.pool_count == 0);
    assert(client.h2_stream_map.free_list == NULL);
    assert(client.h2_stream_map.buckets == client.h2_stream_map.inline_buckets);

    printf("test_h2_stream_resize_and_collision: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Stream Context & Arena Pool Recycling                              */
/* -------------------------------------------------------------------------- */
static void
test_h2_stream_pool_recycling(void)
{
    printf("Testing HTTP/2 stream context and arena pool recycling...\n");

    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    /* Create initial stream */
    csilk_ctx_t* orig_ctx = csilk_h2_get_or_create_stream(&client, 1);
    assert(orig_ctx != NULL);
    csilk_arena_t* orig_arena = orig_ctx->arena;
    assert(orig_arena != NULL);

    /* Allocate memory inside arena */
    char* str = csilk_arena_strdup(orig_ctx->arena, "stream 1 initial arena content");
    assert(str != NULL && strcmp(str, "stream 1 initial arena content") == 0);

    /* Close stream 1 -> should enter free_list */
    assert(csilk_h2_remove_stream(&client, 1) == 0);
    assert(client.h2_stream_map.count == 0);
    assert(client.h2_stream_map.pool_count == 1);
    assert(client.h2_stream_map.free_list == orig_ctx);

    /* Perform 100 consecutive stream cycles, ensuring 100% address reuse and zero leak */
    for (int cycle = 0; cycle < 100; cycle++) {
        int32_t      stream_id = cycle * 2 + 3;
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(c == orig_ctx);
        assert(c->arena == orig_arena);
        assert(c->stream_id == stream_id);
        assert(client.h2_stream_map.pool_count == 0);

        /* Write to arena */
        char* data = csilk_arena_alloc(c->arena, 512);
        assert(data != NULL);
        snprintf(data, 512, "Stream ID %d test payload", stream_id);

        /* Close stream */
        assert(csilk_h2_remove_stream(&client, stream_id) == 0);
        assert(client.h2_stream_map.pool_count == 1);
    }

    csilk_h2_free_streams(&client);
    assert(client.h2_stream_map.pool_count == 0);
    assert(client.h2_stream_map.free_list == NULL);

    printf("test_h2_stream_pool_recycling: PASS\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Pool Lifecycle Benchmark (Acquire + Arena Alloc + Release)          */
/* -------------------------------------------------------------------------- */
static void
benchmark_stream_pool_lifecycle(int iterations)
{
    printf("\n=== Benchmarking Stream Pool Lifecycle (Acquire -> Arena Alloc -> Release) ===\n");

    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    uint64_t c_start = rdtsc();
    double   t_start = now_ns();

    for (int i = 0; i < iterations; i++) {
        int32_t      stream_id = (i % 64) * 2 + 1;
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(&client, stream_id);
        assert(c != NULL);

        /* Use arena */
        void* p = csilk_arena_alloc(c->arena, 128);
        assert(p != NULL);

        /* Close stream (recycles to pool) */
        int r = csilk_h2_remove_stream(&client, stream_id);
        assert(r == 0);
    }

    double   t_elapsed = now_ns() - t_start;
    uint64_t c_elapsed = rdtsc() - c_start;

    double ns_per_cycle = t_elapsed / iterations;
    double cycles_per_cycle = (double)c_elapsed / iterations;
    double cycles_per_sec = (double)iterations / (t_elapsed / 1e9);

    printf("  Pool throughput:   %.2f ns/cycle | %.2f cycles/cycle | %.2f M stream-cycles/sec\n",
           ns_per_cycle,
           cycles_per_cycle,
           cycles_per_sec / 1e6);

    csilk_h2_free_streams(&client);
}

/* -------------------------------------------------------------------------- */
/* Test 5: Benchmark for 100, 1,000, 10,000 Concurrent Streams               */
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
    printf("=== Running HTTP/2 Stream Hash Map & Pool Tests & Benchmarks ===\n\n");

    test_h2_stream_crud();
    test_h2_stream_resize_and_collision();
    test_h2_stream_pool_recycling();

    benchmark_stream_pool_lifecycle(500000);

    benchmark_stream_scale(100, 500000);
    benchmark_stream_scale(1000, 500000);
    benchmark_stream_scale(10000, 500000);

    printf("\n=== All HTTP/2 Stream Hash Map & Pool Tests Passed! ===\n");
    return 0;
}
