/**
 * @file test_hot_reload.c
 * @brief Unit tests for Safe RCU / EBR Router Hot-Reload subsystem.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/server/hot_reload.h"
#include "../../src/core/internal/srv_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define PASS()                                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        tests_passed++;                                                                            \
        printf("  [PASS] %s\n", __func__);                                                         \
    } while (0)

static void
h_v1(csilk_ctx_t* c)
{
    csilk_string(c, 200, "version_1");
}

static void
h_v2(csilk_ctx_t* c)
{
    csilk_string(c, 200, "version_2");
}

/* ------------------------------------------------------------------ */
/* Test 1: Basic RCU acquire and release                              */
/* ------------------------------------------------------------------ */

static void
test_rcu_acquire_release(void)
{
    csilk_router_t* r1 = csilk_router_new();
    csilk_handler_t h1[] = {h_v1, NULL};
    csilk_router_add(r1, "GET", "/api/v1", h1, 1);

    csilk_server_t* s = csilk_server_new(r1);
    assert(s != NULL);

    csilk_rcu_token_t token;
    csilk_router_t*   active = csilk_server_router_acquire(s, &token);
    assert(active == r1);
    assert(token.active == 1);
    assert(token.slot != NULL);

    /* Matching works */
    csilk_handler_t* m = csilk_router_match(active, "GET", "/api/v1");
    assert(m != NULL);
    assert(m[0] == h_v1);

    csilk_server_router_release(s, &token);
    assert(token.active == 0);

    csilk_server_free(s);
    csilk_router_free(r1);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 2: Atomic router swap with in-flight reader                   */
/* ------------------------------------------------------------------ */

static void
test_atomic_router_swap_inflight(void)
{
    csilk_router_t* r1 = csilk_router_new();
    csilk_handler_t h1[] = {h_v1, NULL};
    csilk_router_add(r1, "GET", "/api/v1", h1, 1);

    csilk_server_t* s = csilk_server_new(r1);
    assert(s != NULL);

    /* Reader 1 acquires r1 */
    csilk_rcu_token_t token1;
    csilk_router_t*   held_r1 = csilk_server_router_acquire(s, &token1);
    assert(held_r1 == r1);

    /* Swap to r2 (r1 is retired and scheduled for EBR reclamation) */
    csilk_router_t* r2 = csilk_router_new();
    csilk_handler_t h2[] = {h_v2, NULL};
    csilk_router_add(r2, "GET", "/api/v2", h2, 1);

    csilk_server_set_router(s, r2);

    /* New reader acquires r2 immediately */
    csilk_rcu_token_t token2;
    csilk_router_t*   held_r2 = csilk_server_router_acquire(s, &token2);
    assert(held_r2 == r2);

    /* Reader 1 can still use r1 safely */
    csilk_handler_t* m1 = csilk_router_match(held_r1, "GET", "/api/v1");
    assert(m1 != NULL);
    assert(m1[0] == h_v1);

    /* Reader 2 uses r2 */
    csilk_handler_t* m2 = csilk_router_match(held_r2, "GET", "/api/v2");
    assert(m2 != NULL);
    assert(m2[0] == h_v2);

    /* Reader 1 finishes and releases */
    csilk_server_router_release(s, &token1);

    /* Reader 2 finishes and releases */
    csilk_server_router_release(s, &token2);

    /* Wait for grace period and clean up server */
    csilk_server_wait_grace_period(s);

    csilk_server_free(s);
    csilk_router_free(r2);
    PASS();
}

/* ------------------------------------------------------------------ */
/* Test 3: Multiple rapid generations without reader                  */
/* ------------------------------------------------------------------ */

static void
test_rapid_router_swaps(void)
{
    csilk_router_t* r0 = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r0);
    assert(s != NULL);

    csilk_router_t* last_r = NULL;
    for (int i = 1; i <= 50; i++) {
        csilk_router_t* r = csilk_router_new();
        char            path[32];
        snprintf(path, sizeof(path), "/api/v%d", i);
        csilk_handler_t h[] = {h_v1, NULL};
        csilk_router_add(r, "GET", path, h, 1);
        csilk_server_set_router(s, r);
        last_r = r;
    }

    /* Active router should match the last registered generation */
    csilk_rcu_token_t token;
    csilk_router_t*   active = csilk_server_router_acquire(s, &token);
    assert(active != NULL);
    assert(csilk_router_match(active, "GET", "/api/v50") != NULL);
    assert(csilk_router_match(active, "GET", "/api/v1") == NULL);
    csilk_server_router_release(s, &token);

    csilk_server_free(s);
    if (last_r) {
        csilk_router_free(last_r);
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* Main Runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Safe RCU / EBR Router Hot-Reload Tests ===\n\n");

    test_rcu_acquire_release();
    test_atomic_router_swap_inflight();
    test_rapid_router_swaps();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
