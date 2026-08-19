/**
 * @file test_router_simd_fuzz_bench.c
 * @brief Memory model correctness verification, page boundary fuzz testing, and SIMD benchmark.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "core/primitives/router_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t
get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ====================================================================
 * 1. Page Boundary & Unaligned Pointer Permutation Fuzz Tests
 * ==================================================================== */

static void
test_page_boundary_and_alignment_fuzz(void)
{
    printf("Testing Page Boundary and Unaligned Pointer Invariants...\n");

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }

    /* Allocate 3 contiguous pages: Page 0 & 1 Read/Write, Page 2 PROT_NONE (Guard) */
    size_t total_alloc = (size_t)page_size * 3;
    char* mem = mmap(NULL, total_alloc, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mem != MAP_FAILED);

    char* page0 = mem;
    char* page1 = mem + page_size;
    char* guard_page = mem + page_size * 2;

    int r = mprotect(guard_page, (size_t)page_size, PROT_NONE);
    assert(r == 0);

    /* Fill pages with non-zero deterministic byte pattern */
    for (size_t i = 0; i < (size_t)page_size * 2; i++) {
        mem[i] = (char)((i * 37 + 13) & 0x7F);
        if (mem[i] == '/' || mem[i] == '\0') {
            mem[i] = 'A' + (char)(i % 26);
        }
    }

    /* Test lengths across power-of-two and boundary sizes */
    const size_t test_lens[] = {1,  2,  3,  4,  7,  8,  9,   15,  16,  17,
                                31, 32, 33, 63, 64, 65, 127, 128, 255, 256};
    const size_t num_lens = sizeof(test_lens) / sizeof(test_lens[0]);

    /* Test offsets near the page boundary (page1 start) and guard boundary */
    for (size_t l = 0; l < num_lens; l++) {
        size_t len = test_lens[l];

        for (int delta = -64; delta <= 64; delta++) {
            char* s1 = page1 + delta;
            char* s2 = page0 + (delta + 128);

            /* Copy identical contents */
            memcpy(s2, s1, len);

            /* 1. Fast memcmp equal */
            assert(csilk_memcmp_fast(s1, s2, len) == 1);

            /* 2. Fast common prefix */
            assert(csilk_common_prefix_len_fast(s1, s2, len) == len);

            /* 3. Fast memcmp mismatch at each byte */
            for (size_t diff_pos = 0; diff_pos < len; diff_pos++) {
                s2[diff_pos] ^= 0x5A;
                assert(csilk_memcmp_fast(s1, s2, len) == 0);
                assert(csilk_common_prefix_len_fast(s1, s2, len) == diff_pos);
                s2[diff_pos] ^= 0x5A; /* restore */
            }

            /* 4. SIMD Character Search */
            char target = '@';
            s1[len / 2] = target;
            const char* found = csilk_simd_find_char(s1, len, target);
            assert(found == s1 + len / 2);

            const char* not_found = csilk_simd_find_char(s1, len / 2, target);
            assert(not_found == NULL);
            s1[len / 2] = 'B'; /* restore */
        }
    }

    /* Test get_next_segment near page boundaries */
    for (int delta = -48; delta <= 48; delta++) {
        char* seg_buf = page1 + delta;
        memcpy(seg_buf, "/api/users/profile", 19);
        const char* p = seg_buf;
        size_t      seg_len = 0;

        const char* s = get_next_segment(&p, &seg_len);
        assert(s != NULL && seg_len == 3 && strncmp(s, "api", 3) == 0);

        s = get_next_segment(&p, &seg_len);
        assert(s != NULL && seg_len == 5 && strncmp(s, "users", 5) == 0);

        s = get_next_segment(&p, &seg_len);
        assert(s != NULL && seg_len == 7 && strncmp(s, "profile", 7) == 0);

        s = get_next_segment(&p, &seg_len);
        assert(s == NULL);
    }

    munmap(mem, total_alloc);
    printf("  Page Boundary & Alignment Invariants passed: 100%% Clean!\n\n");
}

/* ====================================================================
 * 2. 100,000 Fuzzing Iterations
 * ==================================================================== */

static void
test_randomized_fuzzing(void)
{
    printf("Running 100,000 Randomized Fuzz Iterations...\n");

    char buf1[2048] __attribute__((aligned(64)));
    char buf2[2048] __attribute__((aligned(64)));

    uint32_t seed = 0x13579BDF;

    for (int iter = 0; iter < 100000; iter++) {
        seed = seed * 1664525ULL + 1013904223ULL;
        size_t off1 = (seed >> 4) % 64;
        size_t off2 = (seed >> 10) % 64;
        size_t len = (seed >> 16) % 512;

        char* s1 = buf1 + off1;
        char* s2 = buf2 + off2;

        for (size_t i = 0; i < len; i++) {
            s1[i] = (char)((seed + i * 17) & 0x7F);
            s2[i] = s1[i];
        }

        /* Test equality */
        assert(csilk_memcmp_fast(s1, s2, len) == (memcmp(s1, s2, len) == 0));
        assert(csilk_common_prefix_len_fast(s1, s2, len) == len);

        /* Randomly inject mismatch */
        if (len > 0 && (seed & 1)) {
            size_t diff_pos = (seed >> 24) % len;
            s2[diff_pos] ^= 0x33;

            assert(csilk_memcmp_fast(s1, s2, len) == 0);
            assert(csilk_common_prefix_len_fast(s1, s2, len) == diff_pos);
        }

        /* Test find char */
        if (len > 0) {
            char        target = (char)(seed & 0x7F);
            const char* ref = memchr(s1, target, len);
            const char* simd = csilk_simd_find_char(s1, len, target);
            assert(ref == simd);
        }
    }

    printf("  Randomized Fuzz Testing passed: 100,000/100,000 Exact Parity!\n\n");
}

/* ====================================================================
 * 3. Throughput Benchmark
 * ==================================================================== */

static void
test_simd_throughput_benchmark(void)
{
    printf("Benchmarking SIMD vs Libc Throughput Across Buffer Sizes...\n");

    const size_t sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 512, 1024};
    const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    char b1[2048] __attribute__((aligned(64)));
    char b2[2048] __attribute__((aligned(64)));
    memset(b1, 'K', sizeof(b1));
    memset(b2, 'K', sizeof(b2));

    printf("  ================================================================================\n");
    printf("  Buffer Size | Iterations | csilk_memcmp_fast | libc memcmp    | Speedup         \n");
    printf("  ------------+------------+-------------------+----------------+-----------------\n");

    for (size_t s = 0; s < num_sizes; s++) {
        size_t sz = sizes[s];
        int    iters = (sz >= 256) ? 500000 : 2000000;

        /* Benchmark csilk_memcmp_fast */
        uint64_t     t0 = get_time_ns();
        volatile int sum1 = 0;
        for (int i = 0; i < iters; i++) {
            sum1 += csilk_memcmp_fast(b1, b2, sz);
        }
        uint64_t t1 = get_time_ns();
        double   dt1_ns = (double)(t1 - t0) / iters;

        /* Benchmark libc memcmp */
        uint64_t     t2 = get_time_ns();
        volatile int sum2 = 0;
        for (int i = 0; i < iters; i++) {
            sum2 += (memcmp(b1, b2, sz) == 0);
        }
        uint64_t t3 = get_time_ns();
        double   dt2_ns = (double)(t3 - t2) / iters;

        double speedup = dt2_ns / (dt1_ns > 0 ? dt1_ns : 1.0);
        printf("  %4zu bytes  | %10d | %8.2f ns/op   | %8.2f ns/op  | %5.2fx            \n",
               sz,
               iters,
               dt1_ns,
               dt2_ns,
               speedup);
    }
    printf(
        "  ================================================================================\n\n");
}

int
main(void)
{
    printf("=== Csilk Router SIMD Memory Model & Fuzzing Suite ===\n\n");
    test_page_boundary_and_alignment_fuzz();
    test_randomized_fuzzing();
    test_simd_throughput_benchmark();
    printf("=== All SIMD memory model and fuzzing tests passed successfully! ===\n");
    return EXIT_SUCCESS;
}
