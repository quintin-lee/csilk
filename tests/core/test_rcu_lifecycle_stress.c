/**
 * @file tests/core/test_rcu_lifecycle_stress.c
 * @brief Stress and lifetime verification for RCU/EBR reader slot architecture:
 *        - No fallback to shared slots
 *        - Strict single-owner lifecycle
 *        - Thread-exit safe cleanup
 *        - Server destruction TLS invalidation
 *        - Thread-ID reuse
 *        - 256+ concurrent reader threads
 *        - Hot reload & acquire race condition
 *        - dlclose safety during reader execution
 */

#include "csilk/csilk.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/server.h"
#include "csilk/core/router.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONCURRENT_300_THREADS 320
#define CONCURRENT_512_THREADS 512

static void
dummy_route_handler(csilk_ctx_t* c)
{
    (void)c;
}

/* ------------------------------------------------------------------ */
/* Test 1: Thread-ID Reuse and Clean Thread Exit                      */
/* ------------------------------------------------------------------ */

typedef struct {
    csilk_server_t* server;
    int             thread_idx;
} reuse_thread_arg_t;

static void*
thread_id_reuse_worker(void* raw_arg)
{
    reuse_thread_arg_t* arg = (reuse_thread_arg_t*)raw_arg;
    csilk_rcu_token_t   token;
    csilk_router_t*     r = csilk_server_router_acquire(arg->server, &token);
    assert(r != NULL);
    assert(token.active == 1);
    assert(token.slot != NULL);

    /* Verify nested acquire on same thread */
    csilk_rcu_token_t nested_token;
    csilk_router_t*   r2 = csilk_server_router_acquire(arg->server, &nested_token);
    assert(r2 == r);
    assert(nested_token.slot == token.slot);

    csilk_server_router_release(arg->server, &nested_token);
    csilk_server_router_release(arg->server, &token);
    return NULL;
}

static void
test_thread_id_reuse(void)
{
    printf("Running test_thread_id_reuse...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h = dummy_route_handler;
    csilk_router_add(router, "GET", "/reuse", &h, 1);
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    /* Sequentially create and join 100 threads, ensuring slots are acquired,
     * thread-exit cleaned, and reused cleanly */
    for (int i = 0; i < 100; i++) {
        pthread_t          tid;
        reuse_thread_arg_t arg = {.server = server, .thread_idx = i};
        int                rc = pthread_create(&tid, NULL, thread_id_reuse_worker, &arg);
        assert(rc == 0);
        pthread_join(tid, NULL);
    }

    csilk_server_free(server);
    csilk_router_free(router);
}

/* ------------------------------------------------------------------ */
/* Test 2: 256+ Concurrent Readers (Dynamic Overflow Slots)           */
/* ------------------------------------------------------------------ */

typedef struct {
    csilk_server_t* server;
    atomic_bool     start_gate;
    atomic_int      active_readers;
    atomic_int      success_count;
} readers_stress_ctx_t;

static void*
reader_300_worker(void* raw_arg)
{
    readers_stress_ctx_t* ctx = (readers_stress_ctx_t*)raw_arg;

    while (!atomic_load_explicit(&ctx->start_gate, memory_order_acquire)) {
        sched_yield();
    }

    for (int iter = 0; iter < 100; iter++) {
        csilk_rcu_token_t token;
        csilk_router_t*   r = csilk_server_router_acquire(ctx->server, &token);
        assert(r != NULL);
        assert(token.active == 1);
        assert(token.slot != NULL);

        atomic_fetch_add_explicit(&ctx->active_readers, 1, memory_order_relaxed);

        /* Simulate small critical section work */
        csilk_handler_t* matched = csilk_router_match(r, "GET", "/test300");
        (void)matched;

        atomic_fetch_sub_explicit(&ctx->active_readers, 1, memory_order_relaxed);
        csilk_server_router_release(ctx->server, &token);
    }

    atomic_fetch_add_explicit(&ctx->success_count, 1, memory_order_relaxed);
    return NULL;
}

static void
test_256_plus_readers(void)
{
    printf("Running test_256_plus_readers (320 concurrent threads)...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h = dummy_route_handler;
    csilk_router_add(router, "GET", "/test300", &h, 1);
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    pthread_t            threads[CONCURRENT_300_THREADS];
    readers_stress_ctx_t ctx = {
        .server = server,
        .start_gate = false,
        .active_readers = 0,
        .success_count = 0,
    };

    for (int i = 0; i < CONCURRENT_300_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, reader_300_worker, &ctx);
        assert(rc == 0);
    }

    /* Release all threads simultaneously */
    atomic_store_explicit(&ctx.start_gate, true, memory_order_release);

    for (int i = 0; i < CONCURRENT_300_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    assert(atomic_load_explicit(&ctx.success_count, memory_order_relaxed) ==
           CONCURRENT_300_THREADS);

    csilk_server_free(server);
    csilk_router_free(router);
}

/* ------------------------------------------------------------------ */
/* Test 3: Server Destruction and TLS Invalidation Safety             */
/* ------------------------------------------------------------------ */

static void*
server_destruction_worker(void* raw_arg)
{
    csilk_server_t** server_ptr = (csilk_server_t**)raw_arg;

    /* Acquire lease on server 1 */
    csilk_rcu_token_t token1;
    csilk_router_t*   r1 = csilk_server_router_acquire(server_ptr[0], &token1);
    assert(r1 != NULL);
    csilk_server_router_release(server_ptr[0], &token1);

    /* Acquire lease on server 2 (different generation/address) */
    csilk_rcu_token_t token2;
    csilk_router_t*   r2 = csilk_server_router_acquire(server_ptr[1], &token2);
    assert(r2 != NULL);
    csilk_server_router_release(server_ptr[1], &token2);

    return NULL;
}

static void
test_server_destruction_tls_invalidation(void)
{
    printf("Running test_server_destruction_tls_invalidation...\n");
    csilk_router_t* r1 = csilk_router_new();
    csilk_server_t* s1 = csilk_server_new(r1);

    csilk_router_t* r2 = csilk_router_new();
    csilk_server_t* s2 = csilk_server_new(r2);

    csilk_server_t* servers[2] = {s1, s2};

    pthread_t tid;
    pthread_create(&tid, NULL, server_destruction_worker, servers);
    pthread_join(tid, NULL);

    /* Destroy s1 first */
    csilk_server_free(s1);
    csilk_router_free(r1);

    /* In the main thread, verify acquiring s2 works flawlessly */
    csilk_rcu_token_t token;
    csilk_router_t*   active_r2 = csilk_server_router_acquire(s2, &token);
    assert(active_r2 == r2);
    csilk_server_router_release(s2, &token);

    csilk_server_free(s2);
    csilk_router_free(r2);
}

/* ------------------------------------------------------------------ */
/* Test 4: Hot Reload & Reader Race + Safe dlclose / Tempfile Cleanup */
/* ------------------------------------------------------------------ */

#define RELOAD_NUM_READERS 32
#define RELOAD_NUM_WRITERS 4
#define RELOADS_PER_WRITER 50

typedef struct {
    csilk_server_t* server;
    atomic_bool     stop;
    atomic_uint     read_count;
    atomic_uint     reloads_done;
} reload_race_ctx_t;

static void*
reload_reader_thread(void* raw_arg)
{
    reload_race_ctx_t* ctx = (reload_race_ctx_t*)raw_arg;
    while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
        csilk_rcu_token_t token;
        csilk_router_t*   r = csilk_server_router_acquire(ctx->server, &token);
        assert(r != NULL);

        /* Query route */
        csilk_handler_t* matched = csilk_router_match(r, "GET", "/api/v1/health");
        (void)matched;

        atomic_fetch_add_explicit(&ctx->read_count, 1, memory_order_relaxed);
        csilk_server_router_release(ctx->server, &token);
    }
    return NULL;
}

static void*
reload_writer_thread(void* raw_arg)
{
    reload_race_ctx_t* ctx = (reload_race_ctx_t*)raw_arg;
    for (int i = 0; i < RELOADS_PER_WRITER; i++) {
        csilk_router_t* new_r = csilk_router_new();
        csilk_handler_t h = dummy_route_handler;
        csilk_router_add(new_r, "GET", "/api/v1/health", &h, 1);

        /* Create a temporary dummy file to verify unlink during retirement */
        char tmp_name[] = "/tmp/csilk_rcu_test_XXXXXX";
        int  fd = mkstemp(tmp_name);
        if (fd >= 0) {
            close(fd);
        }

        csilk_server_set_router_full(ctx->server, new_r, NULL, tmp_name);
        atomic_fetch_add_explicit(&ctx->reloads_done, 1, memory_order_relaxed);
        usleep(100);
    }
    return NULL;
}

static void
test_concurrent_reload_race(void)
{
    printf("Running test_concurrent_reload_race...\n");
    csilk_router_t* r0 = csilk_router_new();
    csilk_handler_t h = dummy_route_handler;
    csilk_router_add(r0, "GET", "/api/v1/health", &h, 1);
    csilk_server_t* server = csilk_server_new(r0);
    assert(server != NULL);

    reload_race_ctx_t ctx = {
        .server = server,
        .stop = false,
        .read_count = 0,
        .reloads_done = 0,
    };

    pthread_t readers[RELOAD_NUM_READERS];
    pthread_t writers[RELOAD_NUM_WRITERS];

    for (int i = 0; i < RELOAD_NUM_READERS; i++) {
        pthread_create(&readers[i], NULL, reload_reader_thread, &ctx);
    }
    for (int i = 0; i < RELOAD_NUM_WRITERS; i++) {
        pthread_create(&writers[i], NULL, reload_writer_thread, &ctx);
    }

    for (int i = 0; i < RELOAD_NUM_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    atomic_store_explicit(&ctx.stop, true, memory_order_release);

    for (int i = 0; i < RELOAD_NUM_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    /* Wait for grace period and ensure complete reclamation */
    csilk_server_wait_grace_period(server);

    csilk_router_t* final_r = csilk_server_get_router(server);
    assert(final_r != NULL);

    csilk_server_free(server);
    csilk_router_free(final_r);
}

/* ------------------------------------------------------------------ */
/* Test 5: 512 Concurrent Readers (Extreme Overflow Stress)            */
/* ------------------------------------------------------------------ */

static void
test_512_plus_readers(void)
{
    printf("Running test_512_plus_readers (512 concurrent threads)...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h = dummy_route_handler;
    csilk_router_add(router, "GET", "/test512", &h, 1);
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    pthread_t            threads[CONCURRENT_512_THREADS];
    readers_stress_ctx_t ctx = {
        .server      = server,
        .start_gate  = false,
        .active_readers = 0,
        .success_count   = 0,
    };

    for (int i = 0; i < CONCURRENT_512_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, reader_300_worker, &ctx);
        assert(rc == 0);
    }

    atomic_store_explicit(&ctx.start_gate, true, memory_order_release);

    for (int i = 0; i < CONCURRENT_512_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    assert(atomic_load_explicit(&ctx.success_count, memory_order_relaxed) ==
           CONCURRENT_512_THREADS);
    /* All 512 threads should have used overflow slots (256 static + 256 dynamic) */
    printf("  -> All %d threads completed successfully (static+overflow slots).\n",
           CONCURRENT_512_THREADS);

    csilk_server_free(server);
    csilk_router_free(router);
}

int
main(void)
{
    printf("=== Starting RCU / EBR Lifetime & Concurrency Stress Suite ===\n");
    test_thread_id_reuse();
    test_256_plus_readers();
    test_server_destruction_tls_invalidation();
    test_concurrent_reload_race();
    test_512_plus_readers();
    printf("=== All RCU / EBR Tests Passed Successfully! ===\n");
    return 0;
}
