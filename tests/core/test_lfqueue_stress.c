/**
 * @file test_lfqueue_stress.c
 * @brief Stress tests and high-throughput benchmarks for C11 intrusive MPSC queue.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "../../src/core/primitives/lfqueue.h"

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
    csilk_lfq_node_t lfq_node;
    uint64_t         seq;
    int              producer_id;
} test_node_t;

/* --- Test 1: Basic FIFO correctness --- */

static void
test_lfq_basic_fifo(void)
{
    printf("[Test 1] Basic FIFO order & re-enqueue correctness...\n");
    csilk_lfqueue_t q;
    csilk_lfq_init(&q);

    test_node_t nodes[100];
    for (int i = 0; i < 100; i++) {
        nodes[i].seq = (uint64_t)i;
        nodes[i].producer_id = 0;
        csilk_lfq_enqueue(&q, &nodes[i].lfq_node);
    }

    for (int i = 0; i < 100; i++) {
        csilk_lfq_node_t* n = csilk_lfq_dequeue(&q);
        assert(n != NULL);
        test_node_t* item = (test_node_t*)n;
        assert(item->seq == (uint64_t)i);
    }

    assert(csilk_lfq_dequeue(&q) == NULL);

    /* Test second round of enqueue / dequeue */
    for (int i = 0; i < 50; i++) {
        nodes[i].seq = (uint64_t)(i + 1000);
        csilk_lfq_enqueue(&q, &nodes[i].lfq_node);
    }

    for (int i = 0; i < 50; i++) {
        csilk_lfq_node_t* n = csilk_lfq_dequeue(&q);
        assert(n != NULL);
        test_node_t* item = (test_node_t*)n;
        assert(item->seq == (uint64_t)(i + 1000));
    }

    assert(csilk_lfq_dequeue(&q) == NULL);
    printf("  -> FIFO & re-enqueue passed.\n");
}

/* --- Test 2: Multithreaded MPSC Stress Test --- */

typedef struct {
    csilk_lfqueue_t* q;
    int              producer_id;
    int              num_items;
    test_node_t*     nodes;
} producer_arg_t;

static void*
producer_thread(void* arg)
{
    producer_arg_t* p = (producer_arg_t*)arg;
    for (int i = 0; i < p->num_items; i++) {
        p->nodes[i].seq = (uint64_t)i;
        p->nodes[i].producer_id = p->producer_id;
        csilk_lfq_enqueue(p->q, &p->nodes[i].lfq_node);
    }
    return NULL;
}

typedef struct {
    csilk_lfqueue_t*     q;
    uint64_t             expected_total;
    atomic_uint_fast64_t count;
    atomic_bool          done;
} consumer_arg_t;

static void*
consumer_thread(void* arg)
{
    consumer_arg_t* c = (consumer_arg_t*)arg;
    uint64_t        dequeued = 0;

    while (dequeued < c->expected_total) {
        csilk_lfq_node_t* n = csilk_lfq_dequeue(c->q);
        if (n) {
            dequeued++;
        } else {
/* Exponential backoff or pause */
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }
    atomic_store_explicit(&c->count, dequeued, memory_order_release);
    atomic_store_explicit(&c->done, true, memory_order_release);
    return NULL;
}

static void
run_mpsc_stress(int num_producers, int items_per_producer)
{
    csilk_lfqueue_t q;
    csilk_lfq_init(&q);

    uint64_t total_items = (uint64_t)num_producers * (uint64_t)items_per_producer;

    pthread_t*      p_threads = malloc(sizeof(pthread_t) * (size_t)num_producers);
    producer_arg_t* p_args = malloc(sizeof(producer_arg_t) * (size_t)num_producers);
    test_node_t**   all_nodes = malloc(sizeof(test_node_t*) * (size_t)num_producers);

    for (int i = 0; i < num_producers; i++) {
        all_nodes[i] = malloc(sizeof(test_node_t) * (size_t)items_per_producer);
        p_args[i].q = &q;
        p_args[i].producer_id = i;
        p_args[i].num_items = items_per_producer;
        p_args[i].nodes = all_nodes[i];
    }

    consumer_arg_t c_arg = {.q = &q, .expected_total = total_items, .count = 0, .done = false};

    pthread_t c_thread;
    pthread_create(&c_thread, NULL, consumer_thread, &c_arg);

    uint64_t start_ns = get_monotonic_ns();

    for (int i = 0; i < num_producers; i++) {
        pthread_create(&p_threads[i], NULL, producer_thread, &p_args[i]);
    }

    for (int i = 0; i < num_producers; i++) {
        pthread_join(p_threads[i], NULL);
    }

    pthread_join(c_thread, NULL);

    uint64_t end_ns = get_monotonic_ns();
    double   secs = (double)(end_ns - start_ns) / 1e9;
    double   mops = (double)total_items / (secs * 1e6);
    double   ns_per_op = (double)(end_ns - start_ns) / (double)total_items;

    assert(atomic_load(&c_arg.count) == total_items);
    assert(csilk_lfq_dequeue(&q) == NULL);

    printf("  [%2d Producers -> 1 Consumer] %8lu items | %6.2f Mops/s (%5.1f ns/op)\n",
           num_producers,
           (unsigned long)total_items,
           mops,
           ns_per_op);

    for (int i = 0; i < num_producers; i++) {
        free(all_nodes[i]);
    }
    free(all_nodes);
    free(p_args);
    free(p_threads);
}

/* --- Test 3: High Scale (1M / 10M ops) Benchmark --- */

static void
run_high_scale_benchmarks(void)
{
    printf("\n=== MPSC Queue High-Scale Benchmark (1,000,000 ~ 10,000,000 items) ===\n");

    /* 1M Single-Threaded enqueue/dequeue benchmark */
    {
        csilk_lfqueue_t q;
        csilk_lfq_init(&q);
        const int    count = 1000000;
        test_node_t* nodes = malloc(sizeof(test_node_t) * (size_t)count);

        uint64_t t0 = get_monotonic_ns();
        for (int i = 0; i < count; i++) {
            csilk_lfq_enqueue(&q, &nodes[i].lfq_node);
        }
        uint64_t t1 = get_monotonic_ns();
        for (int i = 0; i < count; i++) {
            csilk_lfq_node_t* n = csilk_lfq_dequeue(&q);
            assert(n == &nodes[i].lfq_node);
        }
        uint64_t t2 = get_monotonic_ns();

        double enq_mops = (double)count / ((double)(t1 - t0) / 1e3);
        double deq_mops = (double)count / ((double)(t2 - t1) / 1e3);
        printf("  [Single-Threaded 1M] Enqueue: %6.2f Mops/s | Dequeue: %6.2f Mops/s\n",
               enq_mops,
               deq_mops);

        free(nodes);
    }

    /* Multi-Producer Stress Tests */
    run_mpsc_stress(1, 1000000); /* 1M total */
    run_mpsc_stress(4, 250000);  /* 1M total */
    run_mpsc_stress(8, 125000);  /* 1M total */
    run_mpsc_stress(16, 100000); /* 1.6M total */
    run_mpsc_stress(32, 100000); /* 3.2M total */
    run_mpsc_stress(64, 100000); /* 6.4M total */
}

int
main(void)
{
    printf("=== Lock-Free MPSC Queue (lfqueue.h) C11 Memory Model Audit & Benchmark ===\n\n");

    test_lfq_basic_fifo();
    run_high_scale_benchmarks();

    printf("\n=== All lfqueue.h stress tests and benchmarks completed successfully with 0 errors! "
           "===\n");
    return EXIT_SUCCESS;
}
