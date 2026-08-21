/**
 * @file tests/core/test_atomic_lifecycle.c
 * @brief Regression tests for dynamic atomic initialization, lifecycle, and failure unwinding.
 */

#include "core/internal/srv_impl.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/server.h"
#include "csilk/core/sync.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_THREADS 4
#define ITERATIONS 500

typedef struct {
    csilk_server_t* server;
    _Atomic(bool)   running;
    int             thread_id;
} thread_arg_t;

/* --- Test 1: Direct Verification of Atomic Initializers --- */
static void
test_direct_atomic_initializers(void)
{
    /* 1. Runtime config initialization */
    csilk_runtime_config_t rc;
    memset(&rc, 0, sizeof(rc));
    csilk_server_config_t cfg = {
        .idle_timeout_ms = 45000,
        .read_timeout_ms = 5000,
        .write_timeout_ms = 5000,
        .request_timeout_ms = 10000,
        .max_body_size = 1024 * 1024,
        .max_header_size = 16384,
        .max_url_size = 2048,
        .max_headers_count = 64,
        .max_connections = 5000,
        .enable_simd = 1,
        .h2_push_enable = 1,
        .h2_max_push_per_request = 20,
        .backpressure_max_queue_depth = 1000,
        .backpressure_max_latency_us = 50000,
    };
    _csilk_runtime_config_init(&rc, &cfg);

    assert(atomic_load(&rc.idle_timeout_ms) == 45000);
    assert(atomic_load(&rc.read_timeout_ms) == 5000);
    assert(atomic_load(&rc.write_timeout_ms) == 5000);
    assert(atomic_load(&rc.request_timeout_ms) == 10000);
    assert(atomic_load(&rc.max_body_size) == 1024 * 1024);
    assert(atomic_load(&rc.max_header_size) == 16384);
    assert(atomic_load(&rc.max_url_size) == 2048);
    assert(atomic_load(&rc.max_headers_count) == 64);
    assert(atomic_load(&rc.max_connections) == 5000);
    assert(atomic_load(&rc.enable_simd) == 1);
    assert(atomic_load(&rc.h2_push_enable) == 1);
    assert(atomic_load(&rc.h2_max_push_per_request) == 20);
    assert(atomic_load(&rc.backpressure_max_queue_depth) == 1000);
    assert(atomic_load(&rc.backpressure_max_latency_us) == 50000);

    /* 2. Client atomics initialization */
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    _csilk_client_atomics_init(&client);
    assert(atomic_load(&client.ref_count) == 0);
    assert(atomic_load(&client.pending_io) == 0);

    /* 3. Worker pool atomics initialization */
    worker_pool_t wp;
    memset(&wp, 0, sizeof(wp));
    _csilk_worker_pool_atomics_init(&wp, NULL, 1);
    assert(atomic_load(&wp.client_pool_count) == 0);
    assert(atomic_load(&wp.active_connections) == 0);
    assert(atomic_load(&wp.arena_pool_count) == 0);
    for (int t = 0; t < CSILK_READ_BUF_TIER_COUNT; t++) {
        assert(atomic_load(&wp.read_buf_counts[t]) == 0);
    }
}

/* --- Test 2: Server Creation, Failure Unwinding, and Lifecycle --- */
static void
test_server_lifecycle_and_unwinding(void)
{
    csilk_router_t* r = csilk_router_new();
    assert(r != NULL);

    /* Allocate and free multiple times rapidly */
    for (int i = 0; i < 50; i++) {
        csilk_server_t* s = csilk_server_new(r);
        assert(s != NULL);

        /* Verify initialized server atomic state */
        assert(atomic_load(&s->router) == r);
        assert(atomic_load(&s->max_connections) == 0);
        assert(atomic_load(&s->active_connections) == 0);
        for (int h = 0; h < CSILK_HOOK_COUNT; h++) {
            assert(atomic_load(&s->hooks[h]) == NULL);
        }
        assert(atomic_load(&s->runtime_config.idle_timeout_ms) == CSILK_DEFAULT_IDLE_TIMEOUT);
        assert(atomic_load(&s->runtime_config.max_body_size) == CSILK_DEFAULT_MAX_BODY_SIZE);
        assert(atomic_load(&s->runtime_config.max_header_size) == CSILK_DEFAULT_MAX_HEADER_SIZE);

        int active = -1, pooled = -1;
        csilk_server_get_stats(s, &active, &pooled);
        assert(active == 0);
        assert(pooled == 0);

        assert(csilk_server_check_backpressure(s) == 0);

        csilk_server_free(s);
    }

    csilk_router_free(r);
}

/* --- Test 3: Concurrent Multi-Threaded Atomic Stress --- */
static void*
worker_stress_thread(void* arg)
{
    thread_arg_t*   targ = (thread_arg_t*)arg;
    csilk_server_t* s = targ->server;
    int             id = targ->thread_id;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        if (id % 2 == 0) {
            /* Mutator thread */
            csilk_server_config_t cfg = {
                .idle_timeout_ms = (unsigned int)(1000 + (iter % 10) * 100),
                .max_body_size = (size_t)(1024 + iter),
                .max_header_size = (size_t)(512 + (iter % 64)),
                .max_connections = 100 + (iter % 50),
                .backpressure_max_queue_depth = 50 + (size_t)(iter % 20),
            };
            csilk_server_set_config(s, &cfg);
        } else {
            /* Reader / Inspector thread */
            int active = 0, pooled = 0;
            csilk_server_get_stats(s, &active, &pooled);
            assert(active >= 0);
            assert(pooled >= 0);

            int bp = csilk_server_check_backpressure(s);
            (void)bp;

            size_t max_body = _csilk_server_get_max_body_size(s);
            assert(max_body >= 1024);

            size_t max_hdr = _csilk_server_get_max_header_size(s);
            assert(max_hdr >= 512);
        }
    }
    return NULL;
}

static void
test_concurrent_atomic_stress(void)
{
    csilk_router_t* r = csilk_router_new();
    assert(r != NULL);
    csilk_server_t* s = csilk_server_new(r);
    assert(s != NULL);

    pthread_t    threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].server = s;
        args[i].thread_id = i;
        atomic_init(&args[i].running, true);
        pthread_create(&threads[i], NULL, worker_stress_thread, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    csilk_server_free(s);
    csilk_router_free(r);
}

/* --- Test 4: Dynamic Connection Pool & Client Lifecycle Stress --- */
static void
test_client_pool_lifecycle(void)
{
    worker_pool_t wp;
    memset(&wp, 0, sizeof(wp));
    _csilk_worker_pool_atomics_init(&wp, NULL, 0);

    /* Allocate clients using pool_get */
    csilk_client_t* clients[64];
    for (int i = 0; i < 64; i++) {
        clients[i] = pool_get(&wp);
        assert(clients[i] != NULL);
        assert(atomic_load(&clients[i]->ref_count) == 0);
        assert(atomic_load(&clients[i]->pending_io) == 0);

        csilk_client_ref(clients[i]);
        assert(atomic_load(&clients[i]->ref_count) == 1);
        csilk_client_unref(clients[i]);
        assert(atomic_load(&clients[i]->ref_count) == 0);
    }

    /* Release half back to pool */
    for (int i = 0; i < 32; i++) {
        pool_put(&wp, clients[i]);
    }
    assert(atomic_load(&wp.client_pool_count) == 32);

    /* Re-acquire from pool */
    for (int i = 0; i < 32; i++) {
        csilk_client_t* c = pool_get(&wp);
        assert(c != NULL);
        assert(atomic_load(&c->ref_count) == 0);
        assert(atomic_load(&c->pending_io) == 0);
        clients[i] = c;
    }
    assert(atomic_load(&wp.client_pool_count) == 0);

    /* Clean up all clients */
    for (int i = 0; i < 64; i++) {
        free(clients[i]);
    }
}

int
main(void)
{
    printf("Running test_direct_atomic_initializers...\n");
    test_direct_atomic_initializers();

    printf("Running test_server_lifecycle_and_unwinding...\n");
    test_server_lifecycle_and_unwinding();

    printf("Running test_concurrent_atomic_stress...\n");
    test_concurrent_atomic_stress();

    printf("Running test_client_pool_lifecycle...\n");
    test_client_pool_lifecycle();

    printf("All atomic lifecycle tests passed successfully!\n");
    return 0;
}
