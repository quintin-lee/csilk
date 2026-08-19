/**
 * @file test_uring_stale_stress.c
 * @brief Stress test for io_uring 64-bit generation and stale completion detection.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/sys_io.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CSILK_USE_URING

#include "core/uring/uring_internal.h"

#define NUM_ITERATIONS 50000

static atomic_uint_fast64_t g_valid_callbacks = 0;
static atomic_uint_fast64_t g_stale_callbacks = 0;

static void
on_test_timer(csilk_io_timer_t* handle)
{
    uint64_t expected = (uint64_t)(uintptr_t)handle->data;
    if (handle->generation == expected) {
        atomic_fetch_add(&g_valid_callbacks, 1);
    } else {
        atomic_fetch_add(&g_stale_callbacks, 1);
    }
}

static void
test_stale_timer_reclamation(void)
{
    printf("Running stale timer completion stress test (%d iterations)...\n", NUM_ITERATIONS);

    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    csilk_io_timer_t timers[64];
    for (int i = 0; i < 64; i++) {
        csilk_io_timer_init(&loop, &timers[i]);
    }

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        int               idx = iter % 64;
        csilk_io_timer_t* t = &timers[idx];

        /* Set expected generation in data */
        uint64_t target_gen = t->generation + 1;
        t->data = (void*)(uintptr_t)target_gen;

        /* Start timer with short timeout */
        csilk_io_timer_start(t, on_test_timer, 1, 0);

        /* Immediately stop/cancel 50% of the timers to race with fire */
        if ((iter & 1) == 0) {
            csilk_io_timer_stop(t);
            /* Bump generation to simulate recycling */
            t->generation += 10;
        }

        /* Pump loop in nowait mode */
        csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    }

    /* Drain all remaining events */
    for (int i = 0; i < 100; i++) {
        csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
        usleep(500);
    }

    for (int i = 0; i < 64; i++) {
        csilk_io_close((csilk_io_handle_t*)&timers[i], NULL);
    }

    csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    csilk_io_loop_close(&loop);

    printf("  Valid timer executions: %lu, Stale misroutes: %lu\n",
           (unsigned long)atomic_load(&g_valid_callbacks),
           (unsigned long)atomic_load(&g_stale_callbacks));

    assert(atomic_load(&g_stale_callbacks) == 0);
    printf("  Stale timer reclamation test passed!\n");
}

static void
test_op_context_pool_exhaustion(void)
{
    printf("Testing op_context pool dynamic allocation and recycling...\n");

    csilk_io_loop_t loop;
    int             rc = csilk_io_loop_init(&loop);
    assert(rc == 0);

    /* Allocate more contexts than default pool capacity (8192) */
    int                  num_ops = 12000;
    uring_op_context_t** ops = malloc(sizeof(uring_op_context_t*) * (size_t)num_ops);

    for (int i = 0; i < num_ops; i++) {
        ops[i] = uring_op_alloc(&loop);
        assert(ops[i] != NULL);
        ops[i]->generation = (uint64_t)i + 1;
        ops[i]->type = URING_OP_READ;
    }

    /* Verify all operations */
    for (int i = 0; i < num_ops; i++) {
        assert(ops[i]->generation == (uint64_t)i + 1);
        assert(ops[i]->type == URING_OP_READ);
    }

    /* Free all operations back */
    for (int i = 0; i < num_ops; i++) {
        uring_op_free(&loop, ops[i]);
    }

    free(ops);
    csilk_io_loop_close(&loop);
    printf("  Op context pool stress test passed!\n");
}

static void
test_decode_benchmark(void)
{
    printf("Benchmarking user_data direct decode overhead (10,000,000 iterations)...\n");

    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    uring_op_context_t* ctx = uring_op_alloc(&loop);
    ctx->generation = 0x123456789ABCDEF0ULL;
    ctx->type = URING_OP_READ;
    ctx->owner = &loop;
    ctx->data = NULL;

    uint64_t user_data = (uint64_t)(uintptr_t)ctx;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    volatile uint64_t sum = 0;
    for (int i = 0; i < 10000000; i++) {
        uring_op_type_t op = URING_OP_NONE;
        void*           ptr = NULL;
        uint64_t        gen = 0;
        uring_decode_data(user_data, &op, &ptr, &gen);
        sum += gen + (uint64_t)op + (uintptr_t)ptr;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    double ns_per_op = elapsed_ns / 10000000.0;

    printf("  Decode throughput: %.2f Mops/s (%.2f ns/op, sum=%lu)\n",
           1000.0 / ns_per_op,
           ns_per_op,
           (unsigned long)sum);

    uring_op_free(&loop, ctx);
    csilk_io_loop_close(&loop);
}

int
main(void)
{
    printf("=== Linux io_uring 64-Bit Generation & Stale Completion Stress Suite ===\n\n");
    test_stale_timer_reclamation();
    test_op_context_pool_exhaustion();
    test_decode_benchmark();
    printf("\n=== All stale completion stress tests passed successfully! ===\n");
    return EXIT_SUCCESS;
}

#else

int
main(void)
{
    printf("io_uring backend not enabled; skipping uring stale tests.\n");
    return EXIT_SUCCESS;
}

#endif
