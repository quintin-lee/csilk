/**
 * @file tests/core/test_server_config_race.c
 * @brief ThreadSanitizer & concurrency test for dynamic server config updates.
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
#define NUM_UPDATERS 4

typedef struct {
    csilk_server_t* server;
    _Atomic(bool)   running;
    uint64_t        reads_count;
} reader_arg_t;

typedef struct {
    csilk_server_t* server;
    _Atomic(bool)   running;
    uint64_t        updates_count;
} updater_arg_t;

static void*
reader_thread_func(void* raw_arg)
{
    reader_arg_t*   arg = (reader_arg_t*)raw_arg;
    csilk_server_t* s = arg->server;
    uint64_t        count = 0;

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        size_t max_body = _csilk_server_get_max_body_size(s);
        assert(max_body >= 1024);

        size_t max_hdr = _csilk_server_get_max_header_size(s);
        assert(max_hdr >= 512);

        size_t max_url = _csilk_server_get_max_url_size(s);
        (void)max_url;

        unsigned int idle = _csilk_server_get_idle_timeout_ms(s);
        assert(idle >= 1000);

        unsigned int read_t = _csilk_server_get_read_timeout_ms(s);
        (void)read_t;

        unsigned int write_t = _csilk_server_get_write_timeout_ms(s);
        (void)write_t;

        unsigned int req_t = _csilk_server_get_request_timeout_ms(s);
        (void)req_t;

        int simd = _csilk_server_get_enable_simd(s);
        (void)simd;

        int push = _csilk_server_get_h2_push_enable(s);
        (void)push;

        int max_push = _csilk_server_get_h2_max_push(s);
        assert(max_push >= 1);

        int bp = csilk_server_check_backpressure(s);
        (void)bp;

        count++;
    }
    arg->reads_count = count;
    return NULL;
}

static void*
updater_thread_func(void* raw_arg)
{
    updater_arg_t*  arg = (updater_arg_t*)raw_arg;
    csilk_server_t* s = arg->server;
    uint64_t        count = 0;

    csilk_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        cfg.idle_timeout_ms = 5000 + (count % 1000);
        cfg.read_timeout_ms = 2000 + (count % 500);
        cfg.write_timeout_ms = 2000 + (count % 500);
        cfg.request_timeout_ms = 10000 + (count % 2000);
        cfg.max_body_size = 1024 * 1024 * (1 + (count % 8));
        cfg.max_header_size = 8192 * (1 + (count % 4));
        cfg.max_url_size = 4096 * (1 + (count % 2));
        cfg.max_headers_count = 64 + (count % 32);
        cfg.max_connections = 1000 + (count % 500);
        cfg.enable_simd = (count % 2);
        cfg.h2_push_enable = (count % 2);
        cfg.h2_max_push_per_request = 5 + (count % 10);
        cfg.backpressure_max_queue_depth = 500 + (count % 500);

        csilk_server_set_config(s, &cfg);
        csilk_server_set_max_connections(s, (int)(1000 + (count % 100)));
        count++;
    }
    arg->updates_count = count;
    return NULL;
}

static void
test_concurrent_config_updates(void)
{
    printf("Testing concurrent live config updates with readers & updaters...\n");

    csilk_router_t* router = csilk_router_new();
    assert(router != NULL);
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    pthread_t     readers[NUM_WORKERS];
    reader_arg_t  reader_args[NUM_WORKERS];
    pthread_t     updaters[NUM_UPDATERS];
    updater_arg_t updater_args[NUM_UPDATERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        reader_args[i].server = server;
        atomic_init(&reader_args[i].running, true);
        reader_args[i].reads_count = 0;
        assert(pthread_create(&readers[i], NULL, reader_thread_func, &reader_args[i]) == 0);
    }

    for (int i = 0; i < NUM_UPDATERS; i++) {
        updater_args[i].server = server;
        atomic_init(&updater_args[i].running, true);
        updater_args[i].updates_count = 0;
        assert(pthread_create(&updaters[i], NULL, updater_thread_func, &updater_args[i]) == 0);
    }

    /* Stress for 150 ms */
    struct timespec ts = {0, 150 * 1000 * 1000};
    nanosleep(&ts, NULL);

    for (int i = 0; i < NUM_UPDATERS; i++) {
        atomic_store_explicit(&updater_args[i].running, false, memory_order_relaxed);
    }
    for (int i = 0; i < NUM_UPDATERS; i++) {
        pthread_join(updaters[i], NULL);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        atomic_store_explicit(&reader_args[i].running, false, memory_order_relaxed);
    }
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(readers[i], NULL);
    }

    uint64_t total_reads = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        total_reads += reader_args[i].reads_count;
    }
    uint64_t total_updates = 0;
    for (int i = 0; i < NUM_UPDATERS; i++) {
        total_updates += updater_args[i].updates_count;
    }

    printf("  Concurrent Config Test Completed:\n");
    printf("    Total Config Reads:   %" PRIu64 "\n", total_reads);
    printf("    Total Config Updates: %" PRIu64 "\n", total_updates);

    csilk_server_free(server);
    csilk_router_free(router);
}

int
main(void)
{
    csilk_arena_init();
    csilk_reflect_init();
    printf("=================================================================\n");
    printf("         CSILK SERVER CONFIG CONCURRENCY & RACE TEST             \n");
    printf("=================================================================\n\n");

    test_concurrent_config_updates();

    printf("\n=================================================================\n");
    printf("                   TEST COMPLETED SUCCESSFULLY                   \n");
    printf("=================================================================\n");
    return 0;
}
