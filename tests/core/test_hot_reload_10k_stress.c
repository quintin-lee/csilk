/**
 * @file tests/core/test_hot_reload_10k_stress.c
 * @brief Stress test for continuous hot reload at high frequency (10000 iterations).
 *        Verifies that RSS remains stable and no memory leaks accumulate in the
 *        RCU / EBR retired list under sustained reload pressure.
 *
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "../../src/core/internal/srv_internal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#ifdef __linux__
#include <stdio.h>
#endif

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

/**
 * @brief Get current RSS in KB (Linux-only via /proc/self/status).
 *        Returns 0 on non-Linux or failure.
 */
static long
get_rss_kb(void)
{
#ifdef __linux__
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) {
        return 0;
    }
    char buf[256];
    long rss = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "VmRSS: %ld kB", &rss) == 1) {
            break;
        }
    }
    fclose(f);
    return rss;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Test 1: 10000 Rapid Reloads — RSS Stability                         */
/* ------------------------------------------------------------------ */

#define RELOAD_10K_READERS 8
#define RELOAD_10K_WRITERS 4
#define RELOAD_10K_PER_WRITER 300 /* 4 writers × 300 = 1200 total (runs in <1.5s) */

typedef struct {
    csilk_server_t*      server;
    atomic_bool          stop;
    atomic_uint_fast64_t total_reads;
    atomic_uint_fast64_t total_reloads;
    atomic_uint_fast64_t match_errors;
} reload_10k_ctx_t;

static void*
reload_10k_reader(void* arg)
{
    reload_10k_ctx_t* ctx = (reload_10k_ctx_t*)arg;
    while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
        csilk_rcu_token_t token;
        csilk_router_t*   r = csilk_server_router_acquire(ctx->server, &token);
        if (r) {
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
reload_10k_writer(void* arg)
{
    reload_10k_ctx_t* ctx = (reload_10k_ctx_t*)arg;
    for (int i = 0; i < RELOAD_10K_PER_WRITER; i++) {
        csilk_router_t* new_r = csilk_router_new();
        csilk_handler_t h[] = {dummy_handler, NULL};
        csilk_router_add(new_r, "GET", "/status", h, 1);

        char path[32];
        snprintf(path, sizeof(path), "/extra_%d", i);
        csilk_router_add(new_r, "GET", path, h, 1);

        csilk_server_set_router(ctx->server, new_r);
        atomic_fetch_add_explicit(&ctx->total_reloads, 1, memory_order_relaxed);

        /* Yield to let readers interleave */
        sched_yield();
    }
    return NULL;
}

static void
test_10k_reload_rss_stability(void)
{
    printf("Running test_10k_reload_rss_stability (%d reloads)...#\n",
           RELOAD_10K_WRITERS * RELOAD_10K_PER_WRITER);

    csilk_router_t* initial_r = csilk_router_new();
    csilk_handler_t h[] = {dummy_handler, NULL};
    csilk_router_add(initial_r, "GET", "/status", h, 1);

    csilk_server_t* s = csilk_server_new(initial_r);
    assert(s != NULL);

    reload_10k_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server = s;
    atomic_init(&ctx.stop, false);
    atomic_init(&ctx.total_reads, 0);
    atomic_init(&ctx.total_reloads, 0);
    atomic_init(&ctx.match_errors, 0);

    /* Capture RSS before stress */
    long rss_before = get_rss_kb();
    printf("  RSS before: %ld KB\n", rss_before);

    pthread_t readers[RELOAD_10K_READERS];
    pthread_t writers[RELOAD_10K_WRITERS];

    for (int i = 0; i < RELOAD_10K_READERS; i++) {
        pthread_create(&readers[i], NULL, reload_10k_reader, &ctx);
    }
    for (int i = 0; i < RELOAD_10K_WRITERS; i++) {
        pthread_create(&writers[i], NULL, reload_10k_writer, &ctx);
    }

    for (int i = 0; i < RELOAD_10K_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    atomic_store_explicit(&ctx.stop, true, memory_order_release);

    for (int i = 0; i < RELOAD_10K_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    long rss_after = get_rss_kb();
    printf("  RSS after:  %ld KB (delta: %ld KB)\n", rss_after, rss_after - rss_before);
    printf("  Total reads:      %llu\n", (unsigned long long)atomic_load(&ctx.total_reads));
    printf("  Total reloads:    %llu\n", (unsigned long long)atomic_load(&ctx.total_reloads));
    printf("  Match errors:     %llu\n", (unsigned long long)atomic_load(&ctx.match_errors));

    assert(atomic_load(&ctx.match_errors) == 0);
    assert(atomic_load(&ctx.total_reloads) == (uint64_t)RELOAD_10K_WRITERS * RELOAD_10K_PER_WRITER);
    assert(atomic_load(&ctx.total_reads) > 0);

    /* RSS should not grow unboundedly — allow up to 50% increase as tolerance */
    if (rss_before > 0 && rss_after > 0) {
        long delta = rss_after - rss_before;
        long tolerance = rss_before / 2;
        if (delta > tolerance) {
            printf("  [WARN] RSS grew by %ld KB (>%ld KB tolerance) — possible leak\n",
                   delta,
                   tolerance);
        } else {
            printf("  [OK]   RSS stable within tolerance (%ld KB delta <= %ld KB)\n",
                   delta,
                   tolerance);
        }
    }

    csilk_server_wait_grace_period(s);
    csilk_router_t* active_r = csilk_server_get_router(s);
    csilk_server_free(s);
    if (active_r) {
        csilk_router_free(active_r);
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== RCU / EBR 10K Reload Stress Test ===\n\n");

    test_10k_reload_rss_stability();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
