/**
 * @file tests/core/test_server_stats_bench.c
 * @brief Concurrent stress testing & benchmark for csilk_server_get_stats().
 */

#include "core/internal/srv_impl.h"
#include "csilk/reflection/reflect.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/server.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_WORKERS 8
#define NUM_READERS 4

static inline uint64_t
get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    csilk_server_t* server;
    int             worker_index;
    _Atomic(bool)   running;
} worker_bench_arg_t;

typedef struct {
    csilk_server_t* server;
    _Atomic(bool)   running;
    uint64_t        query_count;
} reader_bench_arg_t;

static void*
worker_thread_func(void* raw_arg)
{
    worker_bench_arg_t* arg = (worker_bench_arg_t*)raw_arg;
    worker_pool_t*      wp = &arg->server->worker_pools[arg->worker_index];

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        csilk_client_t* c = pool_get(wp);
        if (c) {
            /* Simulate connection work */
            pool_put(wp, c);
        }
    }
    return NULL;
}

static void*
reader_thread_func(void* raw_arg)
{
    reader_bench_arg_t* arg = (reader_bench_arg_t*)raw_arg;
    csilk_server_t*     s = arg->server;
    uint64_t            count = 0;
    int                 active = 0;
    int                 pooled = 0;

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        csilk_server_get_stats(s, &active, &pooled);
        assert(active >= 0);
        assert(pooled >= 0);
        count++;
    }
    arg->query_count = count;
    return NULL;
}

static void
test_concurrent_stats_stress_and_bench(void)
{
    printf("Testing concurrent multi-worker stats and benchmarking...\n");

    csilk_server_t* server = calloc(1, sizeof(csilk_server_t));
    assert(server != NULL);
    server->worker_pool_count = NUM_WORKERS;
    server->worker_pools = calloc(NUM_WORKERS, sizeof(worker_pool_t));
    assert(server->worker_pools != NULL);

    for (int i = 0; i < NUM_WORKERS; i++) {
        server->worker_pools[i].server = server;
        server->worker_pools[i].worker_index = i;
    }

    _Atomic(bool) running;
    atomic_init(&running, true);

    pthread_t          worker_tids[NUM_WORKERS];
    worker_bench_arg_t worker_args[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_args[i].server = server;
        worker_args[i].worker_index = i;
        atomic_init(&worker_args[i].running, true);
        assert(pthread_create(&worker_tids[i], NULL, worker_thread_func, &worker_args[i]) == 0);
    }

    pthread_t          reader_tids[NUM_READERS];
    reader_bench_arg_t reader_args[NUM_READERS];

    uint64_t start_ns = get_monotonic_ns();

    for (int i = 0; i < NUM_READERS; i++) {
        reader_args[i].server = server;
        atomic_init(&reader_args[i].running, true);
        assert(pthread_create(&reader_tids[i], NULL, reader_thread_func, &reader_args[i]) == 0);
    }

    /* Run for 200 ms */
    struct timespec sleep_time = {0, 200 * 1000 * 1000};
    nanosleep(&sleep_time, NULL);

    for (int i = 0; i < NUM_READERS; i++) {
        atomic_store_explicit(&reader_args[i].running, false, memory_order_relaxed);
    }
    for (int i = 0; i < NUM_READERS; i++) {
        pthread_join(reader_tids[i], NULL);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        atomic_store_explicit(&worker_args[i].running, false, memory_order_relaxed);
    }
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(worker_tids[i], NULL);
    }

    uint64_t total_ns = get_monotonic_ns() - start_ns;
    uint64_t total_queries = 0;
    for (int i = 0; i < NUM_READERS; i++) {
        total_queries += reader_args[i].query_count;
    }

    double total_sec = (double)total_ns / 1000000000.0;
    double qps = (double)total_queries / total_sec;
    double ns_per_query = (double)total_ns / (double)total_queries * NUM_READERS;

    printf("  Concurrent Benchmark Results (%d workers, %d readers):\n", NUM_WORKERS, NUM_READERS);
    printf("    Total Stats Queries: %" PRIu64 "\n", total_queries);
    printf("    Query Throughput:    %.2f M queries/sec\n", qps / 1000000.0);
    printf("    Avg Query Latency:   %.2f ns/query\n\n", ns_per_query);

    for (int i = 0; i < NUM_WORKERS; i++) {
        int pool_cnt =
            atomic_load_explicit(&server->worker_pools[i].client_pool_count, memory_order_relaxed);
        for (int k = 0; k < pool_cnt; k++) {
            free(server->worker_pools[i].client_pool[k]);
        }
    }
    free(server->worker_pools);
    free(server);
}

int
main(void)
{
    csilk_arena_init();
    csilk_reflect_init();
    printf("=================================================================\n");
    printf("        CSILK SERVER STATS CONCURRENCY & BENCHMARK SUITE         \n");
    printf("=================================================================\n\n");

    test_concurrent_stats_stress_and_bench();

    printf("=================================================================\n");
    printf("               SERVER STATS BENCHMARK COMPLETED                  \n");
    printf("=================================================================\n");
    return 0;
}
