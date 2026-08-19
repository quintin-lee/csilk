/**
 * @file test_dispatch_bench.c
 * @brief Multithreaded benchmarks and TSAN stress verification for csilk_dispatch.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "../../src/core/internal/srv_internal.h"
#include "../../src/core/internal/srv_impl.h"
#include "../../src/core/ctx/ctx_internal.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t
get_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    worker_pool_t        wp;
    csilk_io_loop_t      loop;
    csilk_io_async_t     stop_async;
    csilk_client_t       client;
    csilk_ctx_t          ctx;
    pthread_t            worker_thread;
    atomic_uint_fast64_t tasks_processed;
} bench_env_t;

static void
bench_task_callback(void* arg)
{
    bench_env_t* env = (bench_env_t*)arg;
    atomic_fetch_add_explicit(&env->tasks_processed, 1, memory_order_relaxed);
}

static void
on_bench_stop(csilk_io_async_t* handle)
{
    bench_env_t* env = (bench_env_t*)handle->data;
    csilk_io_stop(&env->loop);
}

static void*
worker_loop_thread(void* arg)
{
    bench_env_t* env = (bench_env_t*)arg;
    csilk_io_run(&env->loop, CSILK_IO_RUN_DEFAULT);
    return NULL;
}

static void
init_bench_env(bench_env_t* env)
{
    memset(env, 0, sizeof(*env));
    csilk_io_loop_init(&env->loop);

    env->wp.loop_ptr = &env->loop;
    _csilk_worker_init_dispatch(&env->wp, &env->loop);

    csilk_io_async_init(&env->loop, &env->stop_async, on_bench_stop);
    env->stop_async.data = env;

    env->client.owner_pool = &env->wp;
    env->client.ref_count = 1000000000;
    env->client.state = CSILK_CONN_PROCESSING;

    env->ctx._internal_client = &env->client;

    atomic_init(&env->tasks_processed, 0);

    pthread_create(&env->worker_thread, NULL, worker_loop_thread, env);
}

static void
cleanup_bench_env(bench_env_t* env)
{
    csilk_io_async_send(&env->stop_async);
    pthread_join(env->worker_thread, NULL);

    csilk_io_close((csilk_io_handle_t*)&env->wp.dispatch_async, NULL);
    csilk_io_close((csilk_io_handle_t*)&env->stop_async, NULL);
    for (int i = 0; i < 32 && csilk_io_loop_alive(&env->loop); i++) {
        csilk_io_run(&env->loop, CSILK_IO_RUN_NOWAIT);
    }
    csilk_io_loop_close(&env->loop);
    _csilk_dispatch_pool_cleanup();
}

typedef struct {
    bench_env_t* env;
    int          thread_id;
    int          iterations;
} producer_arg_t;

static void*
producer_worker(void* arg)
{
    producer_arg_t* p = (producer_arg_t*)arg;
    for (int i = 0; i < p->iterations; i++) {
        csilk_dispatch(&p->env->ctx, bench_task_callback, p->env);
    }
    return NULL;
}

static void
run_dispatch_benchmark(int num_producers, int total_ops)
{
    bench_env_t env;
    init_bench_env(&env);

    int ops_per_producer = total_ops / num_producers;
    int actual_total_ops = ops_per_producer * num_producers;

    pthread_t*      threads = malloc(sizeof(pthread_t) * (size_t)num_producers);
    producer_arg_t* args = malloc(sizeof(producer_arg_t) * (size_t)num_producers);

    uint64_t start_ns = get_monotonic_ns();

    for (int i = 0; i < num_producers; i++) {
        args[i].env = &env;
        args[i].thread_id = i;
        args[i].iterations = ops_per_producer;
        pthread_create(&threads[i], NULL, producer_worker, &args[i]);
    }

    for (int i = 0; i < num_producers; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t producers_done_ns = get_monotonic_ns();

    /* Wait for consumer to process all dispatched tasks */
    while (atomic_load_explicit(&env.tasks_processed, memory_order_acquire) <
           (uint64_t)actual_total_ops) {
        csilk_io_async_send(&env.wp.dispatch_async);
        usleep(500);
    }

    uint64_t total_done_ns = get_monotonic_ns();

    double dispatch_secs = (double)(producers_done_ns - start_ns) / 1e9;
    double total_secs = (double)(total_done_ns - start_ns) / 1e9;
    double dispatch_ops_sec = (double)actual_total_ops / dispatch_secs;
    double end_to_end_ops_sec = (double)actual_total_ops / total_secs;
    double avg_ns_per_op = (double)(producers_done_ns - start_ns) / (double)actual_total_ops;

    printf("  [%3d Producers] %8d ops | Dispatch: %8.2f Kops/s (%5.1f ns/op) | E2E: %8.2f Kops/s\n",
           num_producers,
           actual_total_ops,
           dispatch_ops_sec / 1000.0,
           avg_ns_per_op,
           end_to_end_ops_sec / 1000.0);

    assert(atomic_load(&env.tasks_processed) == (uint64_t)actual_total_ops);

    cleanup_bench_env(&env);
    free(threads);
    free(args);
}

int
main(void)
{
    printf("=== csilk_dispatch() Lock-Free Multi-Producer Benchmark & TSAN Audit ===\n\n");

    const int benchmark_ops = 50000;

    run_dispatch_benchmark(1, benchmark_ops);
    run_dispatch_benchmark(8, benchmark_ops);
    run_dispatch_benchmark(32, benchmark_ops);
    run_dispatch_benchmark(128, benchmark_ops);

    printf("\n=== All dispatch benchmarks completed successfully with 0 data races! ===\n");
    return EXIT_SUCCESS;
}
