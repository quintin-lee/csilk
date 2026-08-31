/**
 * @file tests/core/test_arena_bench.c
 * @brief Benchmark arena allocation fast-path across 8, 32, 128, 1024 byte sizes.
 */

#include "csilk/core/server/server.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
static inline uint64_t
read_cpu_cycles(void)
{
    return __rdtsc();
}
#else
static inline uint64_t
read_cpu_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static uint64_t
get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void
benchmark_alloc_size(size_t alloc_size, int num_allocs_per_arena, int iterations)
{
    /* Use a 1MB chunk to test raw fast-path without frequent chunk allocations */
    size_t         chunk_sz = 2 * 1024 * 1024;
    csilk_arena_t* arena = csilk_arena_new(chunk_sz);
    assert(arena != NULL);

    /* Warmup */
    for (int i = 0; i < num_allocs_per_arena; i++) {
        void* p = csilk_arena_alloc(arena, alloc_size);
        assert(p != NULL);
    }
    csilk_arena_reset(arena);

    uint64_t total_allocs = (uint64_t)num_allocs_per_arena * (uint64_t)iterations;

    uint64_t start_cycles = read_cpu_cycles();
    uint64_t start_ns = get_monotonic_ns();

    for (int it = 0; it < iterations; it++) {
        for (int i = 0; i < num_allocs_per_arena; i++) {
            void* p = csilk_arena_alloc(arena, alloc_size);
            /* Prevent compiler from optimizing away the allocation */
            *(volatile uint8_t*)p = 1;
        }
        csilk_arena_reset(arena);
    }

    uint64_t dur_ns = get_monotonic_ns() - start_ns;
    uint64_t dur_cycles = read_cpu_cycles() - start_cycles;

    double ns_per_alloc = (double)dur_ns / (double)total_allocs;
    double cycles_per_alloc = (double)dur_cycles / (double)total_allocs;
    double mops = 1000.0 / ns_per_alloc;

    printf("  %5zu bytes | %6.2f ns/alloc | %6.2f cycles/alloc | %7.1f M ops/sec\n",
           alloc_size,
           ns_per_alloc,
           cycles_per_alloc,
           mops);

    csilk_arena_free(arena);
}

int
main(void)
{
    printf("=================================================================\n");
    printf("              CSILK ARENA FAST-PATH BENCHMARK                    \n");
    printf("=================================================================\n\n");

    printf("  Allocation Size | Latency (ns) | CPU Cycles/alloc | Throughput\n");
    printf("  ----------------+--------------+------------------+------------\n");

    benchmark_alloc_size(8, 4096, 500);
    benchmark_alloc_size(32, 4096, 500);
    benchmark_alloc_size(128, 4096, 500);
    benchmark_alloc_size(1024, 1024, 1000);

    printf("\n=================================================================\n");
    printf("                ARENA BENCHMARK COMPLETED                        \n");
    printf("=================================================================\n");
    return 0;
}
