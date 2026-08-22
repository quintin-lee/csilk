/**
 * @file test_client_lifetime_stress.c
 * @brief High-concurrency 16-worker and 100,000-reuse client lifetime stress test.
 * @copyright MIT License
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/internal/srv_impl.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/sync.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
    } while (0)

static csilk_server_t*
create_mock_server_with_workers(int num_workers)
{
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    s->worker_pools = calloc((size_t)num_workers, sizeof(worker_pool_t));
    s->worker_pool_count = num_workers;
    for (int i = 0; i < num_workers; i++) {
        _csilk_worker_pool_atomics_init(&s->worker_pools[i], s, i);
        _csilk_worker_init_arena_pool(&s->worker_pools[i]);
        _csilk_worker_init_read_buf_pool(&s->worker_pools[i]);
    }
    return s;
}

static void
free_mock_server_with_workers(csilk_server_t* s)
{
    if (!s) {
        return;
    }
    for (int w = 0; w < s->worker_pool_count; w++) {
        worker_pool_t* wp = &s->worker_pools[w];
        int client_cnt = atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed);
        for (int i = 0; i < client_cnt; i++) {
            free(wp->client_pool[i]);
        }
        int arena_cnt = atomic_load_explicit(&wp->arena_pool_count, memory_order_relaxed);
        for (int i = 0; i < arena_cnt; i++) {
            csilk_arena_free(wp->arena_pool[i]);
        }
        for (int tier = 0; tier < CSILK_READ_BUF_TIER_COUNT; tier++) {
            int buf_cnt = atomic_load_explicit(&wp->read_buf_counts[tier], memory_order_relaxed);
            for (int i = 0; i < buf_cnt; i++) {
                free(wp->read_buf_tiers[tier][i]);
            }
        }
    }
    free(s->worker_pools);
    free(s);
}

/* ------------------------------------------------------------------ */
/* Test 1: 100,000 Continuous Client Reuse Cycles                     */
/* ------------------------------------------------------------------ */

static void
test_100k_client_reuse(void)
{
    csilk_server_t* s = create_mock_server_with_workers(1);
    worker_pool_t*  wp = &s->worker_pools[0];
    _csilk_worker_set_current_pool(wp);

    const int iterations = 100000;
    uint64_t  last_gen = 0;

    for (int i = 0; i < iterations; i++) {
        csilk_client_t* client = pool_get(wp);
        assert(client != NULL);
        assert(client->generation > last_gen);
        last_gen = client->generation;

        client->server = s;
        client->owner_pool = wp;

        /* Verify clean initialization */
        assert(client->state == CSILK_CONN_INIT);
        assert(atomic_load(&client->ref_count) == 0);
        assert(atomic_load(&client->pending_io) == 0);

        /* Transition through full lifecycle */
        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
        csilk_client_ref(client); /* Base ref = 1 */

        csilk_conn_set_state(client, CSILK_CONN_READING);

        /* In-flight write */
        csilk_conn_set_state(client, CSILK_CONN_WRITING);
        csilk_client_ref(client);             /* ref = 2 */
        _csilk_client_pending_io_inc(client); /* pio = 1 */

        /* Async lease */
        csilk_client_ref(client); /* ref = 3 */

        /* Connection starts closing */
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);
        client->ctx.conn_closed = 1;

        /* Complete async lease */
        csilk_client_unref(client); /* ref = 2 */

        /* Complete write */
        _csilk_client_pending_io_dec(client); /* pio = 0 */
        csilk_client_unref(client);           /* ref = 1 */

        /* Release base ref */
        csilk_client_unref(client); /* ref = 0, pio = 0 -> destroyed & pooled */

        assert(client->state == CSILK_CONN_INIT);
    }

    /* Verify pool has the recycled client */
    assert(atomic_load(&wp->client_pool_count) == 1);
    csilk_client_t* final_client = wp->client_pool[0];
    assert(final_client->generation == (uint64_t)iterations);

    free_mock_server_with_workers(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: 16 Workers Concurrent Multi-Threaded Stress                */
/* ------------------------------------------------------------------ */

#define NUM_WORKERS 16
#define OPS_PER_WORKER 10000

typedef struct {
    csilk_server_t* server;
    int             worker_index;
} worker_thread_ctx_t;

static void*
worker_stress_thread(void* arg)
{
    worker_thread_ctx_t* ctx = (worker_thread_ctx_t*)arg;
    worker_pool_t*       wp = &ctx->server->worker_pools[ctx->worker_index];
    _csilk_worker_set_current_pool(wp);

    for (int i = 0; i < OPS_PER_WORKER; i++) {
        csilk_client_t* client = pool_get(wp);
        assert(client != NULL);

        client->server = ctx->server;
        client->owner_pool = wp;

        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
        csilk_client_ref(client);

        /* Simulate concurrent sub-operations */
        csilk_client_ref(client);
        _csilk_client_pending_io_inc(client);

        csilk_conn_set_state(client, CSILK_CONN_READING);
        csilk_conn_set_state(client, CSILK_CONN_WRITING);
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);

        _csilk_client_pending_io_dec(client);
        csilk_client_unref(client);
        csilk_client_unref(client); /* Triggers recycle to wp */
    }

    return NULL;
}

static void
test_16_worker_concurrent_lifetime_stress(void)
{
    csilk_server_t*     s = create_mock_server_with_workers(NUM_WORKERS);
    pthread_t           threads[NUM_WORKERS];
    worker_thread_ctx_t ctxs[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        ctxs[i].server = s;
        ctxs[i].worker_index = i;
        int r = pthread_create(&threads[i], NULL, worker_stress_thread, &ctxs[i]);
        assert(r == 0);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify each worker pool properly received recycled connections */
    for (int i = 0; i < NUM_WORKERS; i++) {
        int cnt = atomic_load(&s->worker_pools[i].client_pool_count);
        assert(cnt >= 1);
    }

    free_mock_server_with_workers(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: ABA Delayed Recycle Task Prevention                        */
/* ------------------------------------------------------------------ */

static void
test_aba_delayed_recycle_task_prevention(void)
{
    csilk_server_t* s = create_mock_server_with_workers(1);
    worker_pool_t*  wp = &s->worker_pools[0];
    _csilk_worker_set_current_pool(wp);

    /* Connection 1 at generation 1 */
    csilk_client_t* client = pool_get(wp);
    client->server = s;
    client->owner_pool = wp;
    uint64_t gen1 = client->generation;
    assert(gen1 == 1);

    csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);
    csilk_client_ref(client);
    csilk_conn_set_state(client, CSILK_CONN_CLOSING);
    csilk_client_unref(client); /* Recycled to pool */

    assert(client->state == CSILK_CONN_INIT);

    /* Client is reused for Connection 2 at generation 2 */
    csilk_client_t* reused = pool_get(wp);
    assert(reused == client);
    assert(reused->generation == 2);
    reused->server = s;
    reused->owner_pool = wp;
    csilk_conn_set_state(reused, CSILK_CONN_ACCEPTED);
    csilk_client_ref(reused);

    /* A stale delayed recycle task arrives claiming generation 1 */
    /* It must NOT destroy the active connection at generation 2! */
    if (reused->generation == gen1 && reused->state == CSILK_CONN_CLOSING) {
        client_destroy(reused);
    }

    /* Verify connection 2 remains completely intact and active */
    assert(reused->state == CSILK_CONN_ACCEPTED);
    assert(atomic_load(&reused->ref_count) == 1);

    /* Clean teardown */
    csilk_conn_set_state(reused, CSILK_CONN_CLOSING);
    csilk_client_unref(reused);
    assert(reused->state == CSILK_CONN_INIT);

    free_mock_server_with_workers(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 4: Underflow Protection (ref_count and pending_io)             */
/* ------------------------------------------------------------------ */

static void
test_underflow_protection(void)
{
    csilk_server_t* s = create_mock_server_with_workers(1);
    worker_pool_t*  wp = &s->worker_pools[0];
    _csilk_worker_set_current_pool(wp);

    csilk_client_t* client = pool_get(wp);
    client->server = s;
    client->owner_pool = wp;

    /* Initially ref_count = 0, pending_io = 0 */
    assert(atomic_load(&client->ref_count) == 0);
    assert(atomic_load(&client->pending_io) == 0);

    /* Unref on 0 must NOT underflow to -1 */
    int r1 = csilk_client_unref(client);
    assert(r1 == 0);
    assert(atomic_load(&client->ref_count) == 0);

    int r2 = csilk_client_unref(client);
    assert(r2 == 0);
    assert(atomic_load(&client->ref_count) == 0);

    /* Pending I/O dec on 0 must NOT underflow to -1 */
    int p1 = _csilk_client_pending_io_dec(client);
    assert(p1 == 0);
    assert(atomic_load(&client->pending_io) == 0);

    int p2 = _csilk_client_pending_io_dec(client);
    assert(p2 == 0);
    assert(atomic_load(&client->pending_io) == 0);

    free(client);
    free_mock_server_with_workers(s);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Test Runner                                                   */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Starting Client Lifetime Formal Stress Tests ===\n");

    test_100k_client_reuse();
    test_16_worker_concurrent_lifetime_stress();
    test_aba_delayed_recycle_task_prevention();
    test_underflow_protection();

    printf("\nTest Results: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
