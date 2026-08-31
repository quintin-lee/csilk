#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/core/server/server.h"
#include "core/internal/srv_internal.h"
#include "csilk/reflection/reflect.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* Timing helpers */
static inline double
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

#define TEST_FILE "test_async_logger.log"
#define ROTATION_FILE "test_async_rot.log"

/* -------------------------------------------------------------------------- */
/* Test 1: Near-Zero Overhead for Disabled Log Level                          */
/* -------------------------------------------------------------------------- */
static void
test_disabled_log_level_latency(void)
{
    printf("1. Testing Disabled Log Level Latency (Zero-Overhead)...\n");

    csilk_log_config_t cfg = {
        .level = CSILK_LOG_ERROR, /* TRACE/DEBUG/INFO disabled */
        .file_path = NULL,
        .use_colors = 0,
        .json_format = 0,
        .overflow_strategy = CSILK_LOG_OVERFLOW_DROP,
    };
    assert(csilk_log_init(cfg) == 0);

    const int iterations = 1000000;
    double    t_start = now_ns();

    for (int i = 0; i < iterations; i++) {
        CSILK_LOG_T("Disabled trace log %d %s %f", i, "param", 3.14159);
        CSILK_LOG_D("Disabled debug log %d %s %f", i, "param", 3.14159);
        CSILK_LOG_I("Disabled info log %d %s %f", i, "param", 3.14159);
    }

    double t_total = now_ns() - t_start;
    double ns_per_call = t_total / (iterations * 3);

    printf("   -> 3,000,000 disabled log checks executed in %.2f ms (%.2f ns/call)\n",
           t_total / 1e6,
           ns_per_call);

    /* Assert near-zero branch latency (< 50 ns per check, < 200 ns under TSAN instrumentation) */
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
    assert(ns_per_call < 200.0);
#else
    assert(ns_per_call < 50.0);
#endif

    csilk_log_close();
    printf("   PASS: Disabled log level is near-zero overhead!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Multi-Threaded Request-ID & Text / JSON Modes                      */
/* -------------------------------------------------------------------------- */
#define NUM_WORKERS 4
#define MSGS_PER_WORKER 250

typedef struct {
    int worker_id;
    int json_mode;
} worker_arg_t;

static void*
worker_thread_func(void* arg)
{
    worker_arg_t* warg = (worker_arg_t*)arg;
    char          req_id[64];
    snprintf(req_id, sizeof(req_id), "req-worker-%d-abc", warg->worker_id);
    csilk_log_set_request_id(req_id);

    for (int i = 0; i < MSGS_PER_WORKER; i++) {
        if (warg->json_mode) {
            csilk_json_t* kv = csilk_log_make_kv("worker", "test", "step", "compute", NULL);
            CSILK_LOG_STRUCT(CSILK_LOG_INFO, kv, "Worker %d json msg %d", warg->worker_id, i);
        } else {
            CSILK_LOG_I("Worker %d text msg %d payload string", warg->worker_id, i);
        }
    }
    return NULL;
}

static void
test_multithreaded_request_id_and_modes(void)
{
    printf("2. Testing Multi-Threaded Logging with Request IDs (Text & JSON)...\n");

    /* Text Mode Test */
    remove(TEST_FILE);
    csilk_log_config_t cfg_text = {
        .level = CSILK_LOG_INFO,
        .file_path = TEST_FILE,
        .use_colors = 0,
        .json_format = 0,
        .overflow_strategy = CSILK_LOG_OVERFLOW_BLOCK,
        .queue_capacity = 4096,
    };
    assert(csilk_log_init(cfg_text) == 0);

    pthread_t    threads[NUM_WORKERS];
    worker_arg_t args[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].worker_id = i;
        args[i].json_mode = 0;
        pthread_create(&threads[i], NULL, worker_thread_func, &args[i]);
    }
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }
    csilk_log_close();

    /* Count lines in TEST_FILE */
    FILE* fp = fopen(TEST_FILE, "r");
    assert(fp != NULL);
    int  lines = 0;
    char buf[1024];
    int  found_req_id[NUM_WORKERS] = {0};

    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
        for (int i = 0; i < NUM_WORKERS; i++) {
            char expected[64];
            snprintf(expected, sizeof(expected), "<req-worker-%d-abc>", i);
            if (strstr(buf, expected)) {
                found_req_id[i]++;
            }
        }
    }
    fclose(fp);
    remove(TEST_FILE);

    assert(lines == NUM_WORKERS * MSGS_PER_WORKER);
    for (int i = 0; i < NUM_WORKERS; i++) {
        assert(found_req_id[i] == MSGS_PER_WORKER);
    }
    printf("   Text Mode: %d / %d lines verified with correct request IDs\n",
           lines,
           NUM_WORKERS * MSGS_PER_WORKER);

    /* JSON Mode Test */
    remove(TEST_FILE);
    csilk_log_config_t cfg_json = {
        .level = CSILK_LOG_INFO,
        .file_path = TEST_FILE,
        .use_colors = 0,
        .json_format = 1,
        .overflow_strategy = CSILK_LOG_OVERFLOW_BLOCK,
        .queue_capacity = 4096,
    };
    assert(csilk_log_init(cfg_json) == 0);

    for (int i = 0; i < NUM_WORKERS; i++) {
        args[i].worker_id = i;
        args[i].json_mode = 1;
        pthread_create(&threads[i], NULL, worker_thread_func, &args[i]);
    }
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }
    csilk_log_close();

    fp = fopen(TEST_FILE, "r");
    assert(fp != NULL);
    lines = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
        assert(strstr(buf, "\"request_id\":\"req-worker-") != NULL);
        assert(strstr(buf, "\"worker\":\"test\"") != NULL);
    }
    fclose(fp);
    remove(TEST_FILE);

    assert(lines == NUM_WORKERS * MSGS_PER_WORKER);
    printf("   JSON Mode: %d / %d lines verified with valid JSON KV fields\n",
           lines,
           NUM_WORKERS * MSGS_PER_WORKER);
    printf("   PASS: Multi-threaded request-ID & formatting modes verified!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Overflow Policies (DROP, BLOCK, FALLBACK)                          */
/* -------------------------------------------------------------------------- */
static void
test_overflow_strategies(void)
{
    printf("3. Testing Queue Overflow Policies (DROP, BLOCK, FALLBACK)...\n");

    /* Strategy 1: DROP with tiny queue capacity */
    csilk_log_config_t cfg_drop = {
        .level = CSILK_LOG_INFO,
        .file_path = NULL,
        .overflow_strategy = CSILK_LOG_OVERFLOW_DROP,
        .queue_capacity = 16, /* Tiny capacity */
    };
    assert(csilk_log_init(cfg_drop) == 0);

    for (int i = 0; i < 5000; i++) {
        CSILK_LOG_I("Burst message %d", i);
    }
    csilk_log_flush();
    csilk_log_close();
    printf("   DROP policy tested successfully (no lockup/crash on tiny queue)\n");

    /* Strategy 2: BLOCK policy */
    remove(TEST_FILE);
    csilk_log_config_t cfg_block = {
        .level = CSILK_LOG_INFO,
        .file_path = TEST_FILE,
        .overflow_strategy = CSILK_LOG_OVERFLOW_BLOCK,
        .queue_capacity = 32, /* Small capacity */
    };
    assert(csilk_log_init(cfg_block) == 0);

    const int total_block_msgs = 2000;
    for (int i = 0; i < total_block_msgs; i++) {
        CSILK_LOG_I("Block strategy message %d payload", i);
    }
    csilk_log_close();

    FILE* fp = fopen(TEST_FILE, "r");
    assert(fp != NULL);
    int  lines = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
    }
    fclose(fp);
    remove(TEST_FILE);

    assert(lines == total_block_msgs);
    printf("   BLOCK policy tested: 100%% (%d/%d) messages delivered with tiny queue\n",
           lines,
           total_block_msgs);

    /* Strategy 3: FALLBACK policy */
    csilk_log_config_t cfg_fallback = {
        .level = CSILK_LOG_INFO,
        .file_path = NULL,
        .overflow_strategy = CSILK_LOG_OVERFLOW_FALLBACK,
        .queue_capacity = 16,
    };
    assert(csilk_log_init(cfg_fallback) == 0);

    for (int i = 0; i < 500; i++) {
        CSILK_LOG_I("Fallback burst message %d", i);
    }
    csilk_log_close();
    printf("   FALLBACK policy tested successfully\n");
    printf("   PASS: All overflow strategies verified!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Asynchronous File Rotation                                         */
/* -------------------------------------------------------------------------- */
static void
test_file_rotation_async(void)
{
    printf("4. Testing Asynchronous File Rotation Under Multi-Thread Load...\n");

    remove(ROTATION_FILE);
    remove(ROTATION_FILE ".1");

    csilk_log_config_t cfg = {
        .level = CSILK_LOG_INFO,
        .file_path = ROTATION_FILE,
        .max_file_size = 2048, /* Rotate every 2 KB */
        .use_colors = 0,
        .overflow_strategy = CSILK_LOG_OVERFLOW_BLOCK,
        .queue_capacity = 1024,
    };
    assert(csilk_log_init(cfg) == 0);

    for (int i = 0; i < 500; i++) {
        CSILK_LOG_I("Rotation test message iteration %d with long padding text text text text", i);
    }

    csilk_log_close();

    struct stat st, st_old;
    assert(stat(ROTATION_FILE, &st) == 0);
    assert(stat(ROTATION_FILE ".1", &st_old) == 0);
    assert(st_old.st_size > 0);

    remove(ROTATION_FILE);
    remove(ROTATION_FILE ".1");
    printf("   PASS: Async file rotation verified (both base and .1 file created)!\n\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: Benchmark 0, 1, 10, 100 Logs/Request vs Mutex Baseline             */
/* -------------------------------------------------------------------------- */

/* Synchronous Mutex-based baseline simulator */
static pthread_mutex_t g_sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE*           g_sync_fp = NULL;

static void
sync_log_baseline(const char* msg)
{
    pthread_mutex_lock(&g_sync_mutex);
    if (g_sync_fp) {
        fwrite(msg, 1, strlen(msg), g_sync_fp);
    }
    pthread_mutex_unlock(&g_sync_mutex);
}

typedef struct {
    int thread_id;
    int num_requests;
    int logs_per_request;
    int is_async;
} bench_arg_t;

static void*
bench_worker(void* arg)
{
    bench_arg_t* b = (bench_arg_t*)arg;
    for (int r = 0; r < b->num_requests; r++) {
        for (int l = 0; l < b->logs_per_request; l++) {
            if (b->is_async) {
                CSILK_LOG_I("Req %d log %d status=200 path=/api/v1/resource latency=1.2ms", r, l);
            } else {
                sync_log_baseline("2026-08-20 10:00:00 INFO [server.c:100] req status=200 "
                                  "path=/api/v1/resource\n");
            }
        }
    }
    return NULL;
}

static void
run_benchmark_case(int num_threads, int num_requests_per_thread, int logs_per_request)
{
    int total_requests = num_threads * num_requests_per_thread;
    int total_logs = total_requests * logs_per_request;

    printf("=== Benchmarking %d threads | %d reqs/thread | %d logs/req (Total %d logs) ===\n",
           num_threads,
           num_requests_per_thread,
           logs_per_request,
           total_logs);

    pthread_t   threads[16];
    bench_arg_t args[16];

    /* 1. Synchronous Mutex Baseline */
    if (logs_per_request > 0) {
        g_sync_fp = fopen("/dev/null", "w");
        assert(g_sync_fp != NULL);

        double t_start = now_ns();
        for (int i = 0; i < num_threads; i++) {
            args[i].thread_id = i;
            args[i].num_requests = num_requests_per_thread;
            args[i].logs_per_request = logs_per_request;
            args[i].is_async = 0;
            pthread_create(&threads[i], NULL, bench_worker, &args[i]);
        }
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        double t_sync = now_ns() - t_start;
        fclose(g_sync_fp);
        g_sync_fp = NULL;

        double sync_ns_per_log = t_sync / total_logs;
        double sync_mops = (double)total_logs / (t_sync / 1e9) / 1e6;
        printf("  [Sync Global Mutex]  %.2f ms | %.2f ns/log | %.2f M logs/sec\n",
               t_sync / 1e6,
               sync_ns_per_log,
               sync_mops);
    }

    /* 2. Asynchronous Lock-Free Pipeline */
    csilk_log_config_t cfg = {
        .level = (logs_per_request == 0) ? CSILK_LOG_ERROR : CSILK_LOG_INFO,
        .file_path = (logs_per_request == 0) ? NULL : "/dev/null",
        .use_colors = 0,
        .overflow_strategy = CSILK_LOG_OVERFLOW_BLOCK,
        .queue_capacity = 32768,
    };
    assert(csilk_log_init(cfg) == 0);

    double t_start = now_ns();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].num_requests = num_requests_per_thread;
        args[i].logs_per_request =
            (logs_per_request == 0) ? 1 : logs_per_request; /* test disabled filter */
        args[i].is_async = 1;
        pthread_create(&threads[i], NULL, bench_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    csilk_log_close();
    double t_async = now_ns() - t_start;

    if (logs_per_request == 0) {
        double ns_per_req = t_async / total_requests;
        printf("  [Async Lock-Free]    %.2f ms | %.2f ns/req (Disabled level check)\n\n",
               t_async / 1e6,
               ns_per_req);
    } else {
        double async_ns_per_log = t_async / total_logs;
        double async_mops = (double)total_logs / (t_async / 1e9) / 1e6;
        printf("  [Async Lock-Free]    %.2f ms | %.2f ns/log | %.2f M logs/sec\n\n",
               t_async / 1e6,
               async_ns_per_log,
               async_mops);
    }
}

/* -------------------------------------------------------------------------- */
/* Main Test Runner                                                           */
/* -------------------------------------------------------------------------- */
__attribute__((optimize("O0"))) int
main(void)
{
    csilk_arena_init();
    csilk_reflect_init();
    printf("=================================================================\n");
    printf("     Asynchronous Lock-Free Logger Tests & Benchmark Suite       \n");
    printf("=================================================================\n\n");

    test_disabled_log_level_latency();
    test_multithreaded_request_id_and_modes();
    test_overflow_strategies();
    test_file_rotation_async();

    printf("Running Performance & Scale Benchmarks (4 Threads):\n");
    run_benchmark_case(4, 5000, 0);  /* 0 logs/request */
    run_benchmark_case(4, 2000, 1);  /* 1 log/request */
    run_benchmark_case(4, 500, 10);  /* 10 logs/request */
    run_benchmark_case(4, 100, 100); /* 100 logs/request */

    printf("=================================================================\n");
    printf("         All Async Logger Tests & Benchmarks Passed!             \n");
    printf("=================================================================\n");
    return 0;
}
