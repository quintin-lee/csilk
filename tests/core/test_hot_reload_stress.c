/**
 * @file test_hot_reload_stress.c
 * @brief Stress tests and dynamic library live reload for Safe RCU / EBR Router.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/server/hot_reload.h"
#include "../../src/core/internal/srv_internal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static int tests_run = 0;
static int tests_passed = 0;

#define PASS()                                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        tests_passed++;                                                                            \
        printf("  [PASS] %s\n", __func__);                                                         \
    } while (0)

static void
dummy_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "ok");
}

/* ------------------------------------------------------------------ */
/* Test 1: High-Concurrency Multi-Threaded Continuous Reload Stress    */
/* ------------------------------------------------------------------ */

#define STRESS_NUM_READERS 8
#define STRESS_NUM_WRITERS 2
#define STRESS_SWAPS_PER_WRITER 100

typedef struct {
    csilk_server_t*      server;
    atomic_bool          stop;
    atomic_uint_fast64_t total_reads;
    atomic_uint_fast64_t total_reloads;
    atomic_uint_fast64_t match_errors;
} stress_ctx_t;

static void*
stress_reader_worker(void* arg)
{
    stress_ctx_t* ctx = (stress_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
        csilk_rcu_token_t token;
        csilk_router_t*   r = csilk_server_router_acquire(ctx->server, &token);
        if (r) {
            /* Verify route matching inside read critical section */
            csilk_handler_t* m = csilk_router_match(r, "GET", "/status");
            if (!m) {
                atomic_fetch_add_explicit(&ctx->match_errors, 1, memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&ctx->total_reads, 1, memory_order_relaxed);
        }
        csilk_server_router_release(ctx->server, &token);
    }
    return NULL;
}

static void*
stress_reloader_worker(void* arg)
{
    stress_ctx_t* ctx = (stress_ctx_t*)arg;
    for (int i = 0; i < STRESS_SWAPS_PER_WRITER; i++) {
        csilk_router_t* new_r = csilk_router_new();
        csilk_handler_t h[] = {dummy_handler, NULL};
        csilk_router_add(new_r, "GET", "/status", h, 1);

        char extra_path[32];
        snprintf(extra_path, sizeof(extra_path), "/extra_%d", i);
        csilk_router_add(new_r, "GET", extra_path, h, 1);

        csilk_server_set_router(ctx->server, new_r);
        atomic_fetch_add_explicit(&ctx->total_reloads, 1, memory_order_relaxed);
        usleep(500); /* 0.5 ms */
    }
    return NULL;
}

static void
test_concurrent_reload_stress(void)
{
    csilk_router_t* initial_r = csilk_router_new();
    csilk_handler_t h[] = {dummy_handler, NULL};
    csilk_router_add(initial_r, "GET", "/status", h, 1);

    csilk_server_t* s = csilk_server_new(initial_r);
    assert(s != NULL);

    stress_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = s;
    atomic_init(&ctx.stop, false);
    atomic_init(&ctx.total_reads, 0);
    atomic_init(&ctx.total_reloads, 0);
    atomic_init(&ctx.match_errors, 0);

    pthread_t readers[STRESS_NUM_READERS];
    pthread_t writers[STRESS_NUM_WRITERS];

    for (int i = 0; i < STRESS_NUM_READERS; i++) {
        pthread_create(&readers[i], NULL, stress_reader_worker, &ctx);
    }
    for (int i = 0; i < STRESS_NUM_WRITERS; i++) {
        pthread_create(&writers[i], NULL, stress_reloader_worker, &ctx);
    }

    for (int i = 0; i < STRESS_NUM_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    atomic_store_explicit(&ctx.stop, true, memory_order_relaxed);

    for (int i = 0; i < STRESS_NUM_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    assert(atomic_load(&ctx.match_errors) == 0);
    assert(atomic_load(&ctx.total_reloads) == STRESS_NUM_WRITERS * STRESS_SWAPS_PER_WRITER);
    assert(atomic_load(&ctx.total_reads) > 0);

    csilk_server_wait_grace_period(s);
    csilk_router_t* active_r = csilk_server_get_router(s);
    csilk_server_free(s);
    if (active_r) {
        csilk_router_free(active_r);
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: Slow Worker with Continuous Concurrent Reloads (Req 12)   */
/* ------------------------------------------------------------------ */

typedef struct {
    csilk_server_t* server;
    atomic_bool     slow_worker_started;
    atomic_bool     slow_worker_done;
    atomic_bool     slow_worker_success;
} slow_worker_ctx_t;

static void*
slow_worker_thread(void* arg)
{
    slow_worker_ctx_t* ctx = (slow_worker_ctx_t*)arg;

    csilk_rcu_token_t token;
    csilk_router_t*   r = csilk_server_router_acquire(ctx->server, &token);
    assert(r != NULL);

    /* Signal that slow worker is inside the critical section with initial router */
    atomic_store_explicit(&ctx->slow_worker_started, true, memory_order_release);

    /* Simulate long-running handler execution (50 ms) */
    usleep(50000);

    /* Verify router is still intact and safe to read */
    csilk_handler_t* m = csilk_router_match(r, "GET", "/slow_endpoint");
    if (m != NULL && m[0] == dummy_handler) {
        atomic_store_explicit(&ctx->slow_worker_success, true, memory_order_relaxed);
    }

    csilk_server_router_release(ctx->server, &token);
    atomic_store_explicit(&ctx->slow_worker_done, true, memory_order_relaxed);
    return NULL;
}

static void
test_slow_worker_with_continuous_reloads(void)
{
    csilk_router_t* r_initial = csilk_router_new();
    csilk_handler_t h[] = {dummy_handler, NULL};
    csilk_router_add(r_initial, "GET", "/slow_endpoint", h, 1);

    csilk_server_t* s = csilk_server_new(r_initial);
    assert(s != NULL);

    slow_worker_ctx_t ctx;
    ctx.server = s;
    atomic_init(&ctx.slow_worker_started, false);
    atomic_init(&ctx.slow_worker_done, false);
    atomic_init(&ctx.slow_worker_success, false);

    pthread_t slow_th;
    pthread_create(&slow_th, NULL, slow_worker_thread, &ctx);

    /* Wait for slow worker to acquire r_initial before starting reloads */
    while (!atomic_load_explicit(&ctx.slow_worker_started, memory_order_acquire)) {
        usleep(100);
    }

    /* While slow worker is in critical section, perform 50 continuous rapid reloads */
    csilk_router_t* last_r = NULL;
    for (int i = 0; i < 50; i++) {
        csilk_router_t* new_r = csilk_router_new();
        char            path[32];
        snprintf(path, sizeof(path), "/new_endpoint_%d", i);
        csilk_handler_t h_new[] = {dummy_handler, NULL};
        csilk_router_add(new_r, "GET", path, h_new, 1);
        csilk_server_set_router(s, new_r);
        last_r = new_r;
        usleep(1000); /* 1 ms */
    }

    pthread_join(slow_th, NULL);

    assert(atomic_load(&ctx.slow_worker_done) == true);
    assert(atomic_load(&ctx.slow_worker_success) == true);

    csilk_server_wait_grace_period(s);
    csilk_server_free(s);
    if (last_r) {
        csilk_router_free(last_r);
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: Dynamic Shared Library Hot-Reload (Req 14)                 */
/* ------------------------------------------------------------------ */

static void
test_dynamic_library_reload(void)
{
    const char* src_file = "/tmp/csilk_test_plugin.c";
    const char* so_file = "/tmp/csilk_test_plugin.so";

    /* Step 1: Create plugin version 1 source */
    FILE* f1 = fopen(src_file, "w");
    assert(f1 != NULL);
    fprintf(f1,
            "#include <csilk/csilk.h>\n"
            "#include <string.h>\n"
            "static void plugin_handler_v1(csilk_ctx_t* c) {\n"
            "    csilk_string(c, 200, \"RESPONSE_V1\");\n"
            "}\n"
            "csilk_router_t* test_plugin_init(void) {\n"
            "    csilk_router_t* r = csilk_router_new();\n"
            "    static csilk_handler_t h[] = {plugin_handler_v1, NULL};\n"
            "    csilk_router_add(r, \"GET\", \"/plugin_route\", h, 1);\n"
            "    return r;\n"
            "}\n");
    fclose(f1);

    /* Compile plugin v1 */
    char        compile_cmd[1024];
    char        cwd_buf[512];
    const char* cwd = getcwd(cwd_buf, sizeof(cwd_buf));
    if (!cwd) {
        cwd = ".";
    }
    snprintf(compile_cmd,
             sizeof(compile_cmd),
             "cc -shared -fPIC -Iinclude -Ibuild/include -Ibuild_uring/include -Isrc/core %s "
             "-Lbuild -Lbuild_uring -lcsilk -Wl,-rpath,%s/build -Wl,-rpath,%s/build_uring -o %s",
             src_file,
             cwd,
             cwd,
             so_file);
    int rc = system(compile_cmd);
    if (rc != 0) {
        printf("  [SKIP] Compiler unavailable or failed for dynamic library test (cmd: %s)\n",
               compile_cmd);
        unlink(src_file);
        return;
    }

    /* Start server and attach hot reload watcher */
    csilk_router_t* r_base = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r_base);
    assert(s != NULL);

    int start_rc = csilk_dev_hot_reload_start(s, so_file, "test_plugin_init");
    assert(start_rc == 0);
    /* r_base was retired into EBR during the initial load_and_swap_router */

    /* Verify router is now on version 1 */
    csilk_rcu_token_t t1;
    csilk_router_t*   r1 = csilk_server_router_acquire(s, &t1);
    assert(r1 != NULL);
    csilk_handler_t* m1 = csilk_router_match(r1, "GET", "/plugin_route");
    assert(m1 != NULL);
    csilk_server_router_release(s, &t1);

    /* Step 2: Update plugin source to version 2 */
    FILE* f2 = fopen(src_file, "w");
    assert(f2 != NULL);
    fprintf(f2,
            "#include <csilk/csilk.h>\n"
            "#include <string.h>\n"
            "static void plugin_handler_v2(csilk_ctx_t* c) {\n"
            "    csilk_string(c, 200, \"RESPONSE_V2\");\n"
            "}\n"
            "csilk_router_t* test_plugin_init(void) {\n"
            "    csilk_router_t* r = csilk_router_new();\n"
            "    static csilk_handler_t h[] = {plugin_handler_v2, NULL};\n"
            "    csilk_router_add(r, \"GET\", \"/plugin_route_v2\", h, 1);\n"
            "    return r;\n"
            "}\n");
    fclose(f2);

    /* Recompile plugin to the same target .so path */
    rc = system(compile_cmd);
    assert(rc == 0);

    /* Trigger hot reload */
    int trigger_rc = csilk_dev_hot_reload_trigger(s);
    assert(trigger_rc == 0);

    /* Verify router has updated to version 2 */
    csilk_rcu_token_t t2;
    csilk_router_t*   r2 = csilk_server_router_acquire(s, &t2);
    assert(r2 != NULL);
    csilk_handler_t* m2 = csilk_router_match(r2, "GET", "/plugin_route_v2");
    assert(m2 != NULL);
    csilk_server_router_release(s, &t2);

    csilk_dev_hot_reload_stop(s);
    csilk_server_wait_grace_period(s);
    csilk_server_free(s);

    unlink(src_file);
    unlink(so_file);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Safe RCU / EBR Router Hot-Reload Stress & Dynamic SO Tests ===\n\n");

    test_concurrent_reload_stress();
    test_slow_worker_with_continuous_reloads();
    test_dynamic_library_reload();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
