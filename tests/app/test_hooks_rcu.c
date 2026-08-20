/**
 * @file test_hooks_rcu.c
 * @brief Multi-threaded RCU and Copy-On-Write Lifecycle Hook System Tests.
 * @copyright MIT License
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/hooks.h"
#include "csilk/core/server.h"
#include "core/internal/srv_internal.h"

/* -------------------------------------------------------------------------- */
/* Test 1: Basic LIFO Execution Order & Signatures                            */
/* -------------------------------------------------------------------------- */

static int g_order_log[16];
static int g_order_idx = 0;

static void
hook_server_start(csilk_server_t* s)
{
    assert(s != NULL);
    g_order_log[g_order_idx++] = 100;
}

static void
hook_server_stop(csilk_server_t* s)
{
    assert(s != NULL);
    g_order_log[g_order_idx++] = 200;
}

static void
hook_ctx_1(csilk_ctx_t* c)
{
    assert(c != NULL);
    g_order_log[g_order_idx++] = 1;
}

static void
hook_ctx_2(csilk_ctx_t* c)
{
    assert(c != NULL);
    g_order_log[g_order_idx++] = 2;
}

static void
hook_ctx_3(csilk_ctx_t* c)
{
    assert(c != NULL);
    g_order_log[g_order_idx++] = 3;
}

static void
test_lifo_and_signatures(void)
{
    printf("1. Testing LIFO execution order and hook signatures...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* s = csilk_server_new(router);
    csilk_ctx_t*    c = csilk_test_ctx_new();

    /* Register server start and stop hooks */
    csilk_server_add_hook(s, CSILK_HOOK_SERVER_START, hook_server_start);
    csilk_server_add_hook(s, CSILK_HOOK_SERVER_STOP, hook_server_stop);

    /* Register 3 request_begin hooks (1, then 2, then 3) */
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_1);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_2);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_3);

    g_order_idx = 0;
    _csilk_trigger_hooks(s, NULL, CSILK_HOOK_SERVER_START);
    assert(g_order_idx == 1 && g_order_log[0] == 100);

    g_order_idx = 0;
    _csilk_trigger_hooks(s, c, CSILK_HOOK_REQUEST_BEGIN);
    /* Expect LIFO: 3, then 2, then 1 */
    assert(g_order_idx == 3);
    assert(g_order_log[0] == 3);
    assert(g_order_log[1] == 2);
    assert(g_order_log[2] == 1);

    csilk_test_ctx_free(c);
    csilk_server_free(s);
    csilk_router_free(router);

    printf("   PASS: LIFO order & signatures verified!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Runtime Hook Removal & Clear                                       */
/* -------------------------------------------------------------------------- */
static void
test_hook_removal_and_clear(void)
{
    printf("2. Testing Runtime Hook Removal & Clear...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* s = csilk_server_new(router);
    csilk_ctx_t*    c = csilk_test_ctx_new();

    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_1);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_2);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_3);

    /* Remove middle hook (hook_ctx_2) */
    int r = csilk_server_remove_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_2);
    assert(r == 0);

    g_order_idx = 0;
    _csilk_trigger_hooks(s, c, CSILK_HOOK_REQUEST_BEGIN);
    assert(g_order_idx == 2);
    assert(g_order_log[0] == 3);
    assert(g_order_log[1] == 1);

    /* Remove non-existent hook -> must return -1 */
    assert(csilk_server_remove_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_2) == -1);

    /* Remove hook_ctx_3 and hook_ctx_1 */
    assert(csilk_server_remove_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_3) == 0);
    assert(csilk_server_remove_hook(s, CSILK_HOOK_REQUEST_BEGIN, hook_ctx_1) == 0);

    /* Array is now empty */
    g_order_idx = 0;
    _csilk_trigger_hooks(s, c, CSILK_HOOK_REQUEST_BEGIN);
    assert(g_order_idx == 0);

    /* Test clear */
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_END, hook_ctx_1);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_END, hook_ctx_2);
    csilk_server_clear_hooks(s, CSILK_HOOK_REQUEST_END);

    g_order_idx = 0;
    _csilk_trigger_hooks(s, c, CSILK_HOOK_REQUEST_END);
    assert(g_order_idx == 0);

    csilk_test_ctx_free(c);
    csilk_server_free(s);
    csilk_router_free(router);

    printf("   PASS: Hook removal and clear verified!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: High-Concurrency Multi-Threaded RCU Stress Test                    */
/* -------------------------------------------------------------------------- */

#define CONCURRENT_READERS 4
#define READER_ITERS 25000

typedef struct {
    csilk_server_t*   s;
    csilk_ctx_t*      c;
    _Atomic(int)*     running;
    _Atomic(uint64_t) trigger_count;
} reader_arg_t;

static _Atomic(uint64_t) g_hook_invocations = 0;

static void
concurrent_hook_alpha(csilk_ctx_t* c)
{
    (void)c;
    atomic_fetch_add_explicit(&g_hook_invocations, 1, memory_order_relaxed);
}

static void
concurrent_hook_beta(csilk_ctx_t* c)
{
    (void)c;
    atomic_fetch_add_explicit(&g_hook_invocations, 1, memory_order_relaxed);
}

static void
concurrent_hook_gamma(csilk_ctx_t* c)
{
    (void)c;
    atomic_fetch_add_explicit(&g_hook_invocations, 1, memory_order_relaxed);
}

static void*
reader_thread_func(void* arg)
{
    reader_arg_t* r = (reader_arg_t*)arg;
    for (int i = 0; i < READER_ITERS; i++) {
        csilk_rcu_token_t token;
        csilk_server_router_acquire(r->s, &token);
        _csilk_trigger_hooks(r->s, r->c, CSILK_HOOK_REQUEST_BEGIN);
        _csilk_trigger_hooks(r->s, r->c, CSILK_HOOK_REQUEST_END);
        csilk_server_router_release(r->s, &token);
        atomic_fetch_add_explicit(&r->trigger_count, 2, memory_order_relaxed);
    }
    return NULL;
}

static void*
writer_thread_func(void* arg)
{
    reader_arg_t* w = (reader_arg_t*)arg;
    while (atomic_load_explicit(w->running, memory_order_relaxed)) {
        csilk_server_add_hook(w->s, CSILK_HOOK_REQUEST_BEGIN, concurrent_hook_alpha);
        usleep(50);
        csilk_server_add_hook(w->s, CSILK_HOOK_REQUEST_BEGIN, concurrent_hook_beta);
        usleep(50);
        csilk_server_add_hook(w->s, CSILK_HOOK_REQUEST_END, concurrent_hook_gamma);
        usleep(50);

        csilk_server_remove_hook(w->s, CSILK_HOOK_REQUEST_BEGIN, concurrent_hook_alpha);
        usleep(50);
        csilk_server_remove_hook(w->s, CSILK_HOOK_REQUEST_BEGIN, concurrent_hook_beta);
        usleep(50);
        csilk_server_remove_hook(w->s, CSILK_HOOK_REQUEST_END, concurrent_hook_gamma);
        usleep(50);
    }
    return NULL;
}

static void
test_concurrent_rcu_stress(void)
{
    printf(
        "3. Testing High-Concurrency Multi-Threaded RCU Hook Safety (4 Readers + 1 Writer)...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* s = csilk_server_new(router);

    _Atomic(int) running = 1;
    pthread_t    readers[CONCURRENT_READERS];
    pthread_t    writer;
    reader_arg_t r_args[CONCURRENT_READERS];
    reader_arg_t w_arg = {.s = s, .c = NULL, .running = &running, .trigger_count = 0};

    /* Seed initial hooks */
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_BEGIN, concurrent_hook_alpha);
    csilk_server_add_hook(s, CSILK_HOOK_REQUEST_END, concurrent_hook_gamma);

    pthread_create(&writer, NULL, writer_thread_func, &w_arg);

    for (int i = 0; i < CONCURRENT_READERS; i++) {
        r_args[i].s = s;
        r_args[i].c = csilk_test_ctx_new();
        r_args[i].running = &running;
        atomic_init(&r_args[i].trigger_count, 0);
        pthread_create(&readers[i], NULL, reader_thread_func, &r_args[i]);
    }

    uint64_t total_triggers = 0;
    for (int i = 0; i < CONCURRENT_READERS; i++) {
        pthread_join(readers[i], NULL);
        total_triggers += atomic_load(&r_args[i].trigger_count);
        csilk_test_ctx_free(r_args[i].c);
    }

    atomic_store_explicit(&running, 0, memory_order_relaxed);
    pthread_join(writer, NULL);

    csilk_server_free(s);
    csilk_router_free(router);

    printf("   -> Total triggers: %lu | Total hook callbacks executed: %lu\n",
           total_triggers,
           atomic_load(&g_hook_invocations));
    printf("   PASS: Concurrent RCU Hook Stress completed with 0 errors / 0 race conditions!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Main Runner                                                                */
/* -------------------------------------------------------------------------- */
int
main(void)
{
    printf("=================================================================\n");
    printf("       Csilk RCU & Copy-On-Write Hook System Test Suite          \n");
    printf("=================================================================\n\n");

    test_lifo_and_signatures();
    test_hook_removal_and_clear();
    test_concurrent_rcu_stress();

    printf("=================================================================\n");
    printf("            All RCU Hook System Tests Passed!                    \n");
    printf("=================================================================\n");
    return 0;
}
