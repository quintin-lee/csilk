#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"
#include "core/internal/srv_internal.h"

static int g_trace_events[32];
static int g_trace_count = 0;

static void
reset_trace(void)
{
    memset(g_trace_events, 0, sizeof(g_trace_events));
    g_trace_count = 0;
}

static void
push_trace(int id)
{
    if (g_trace_count < 32) {
        g_trace_events[g_trace_count++] = id;
    }
}

/* Middleware & handlers for full onion chain test */
static void
mw_global_1(csilk_ctx_t* c)
{
    push_trace(1);
    csilk_next(c);
    push_trace(101);
}

static void
mw_global_2(csilk_ctx_t* c)
{
    push_trace(2);
    csilk_next(c);
    push_trace(102);
}

static void
mw_group_parent(csilk_ctx_t* c)
{
    push_trace(3);
    csilk_next(c);
    push_trace(103);
}

static void
mw_group_child(csilk_ctx_t* c)
{
    push_trace(4);
    csilk_next(c);
    push_trace(104);
}

static void
mw_route(csilk_ctx_t* c)
{
    push_trace(5);
    csilk_next(c);
    push_trace(105);
}

static void
handler_target(csilk_ctx_t* c)
{
    push_trace(6);
    csilk_string(c, CSILK_STATUS_OK, "chain_complete");
}

/* Short-circuiting middleware */
static void
mw_short_circuit(csilk_ctx_t* c)
{
    push_trace(99);
    csilk_string(c, CSILK_STATUS_FORBIDDEN, "blocked");
    /* Do not call csilk_next(c) */
}

/* Abort middleware */
static void
mw_abort(csilk_ctx_t* c)
{
    push_trace(88);
    csilk_abort(c);
    csilk_next(c); /* Should be no-op */
}

/* Test 1: Onion execution order and zero-copy pointer match */
static void
test_full_chain_order_and_zero_alloc(void)
{
    printf("Testing full middleware chain onion order and zero copy...\n");

    csilk_router_t* r = csilk_router_new();
    assert(r != NULL);

    csilk_server_t* s = csilk_server_new(r);
    assert(s != NULL);

    /* 1. Register global middlewares */
    csilk_server_use(s, mw_global_1);
    csilk_server_use(s, mw_global_2);

    /* 2. Register parent & child groups */
    csilk_group_t* parent_grp = csilk_group_new(r, "/api");
    csilk_group_use(parent_grp, mw_group_parent);

    csilk_group_t* child_grp = csilk_group_group(parent_grp, "/v1");
    csilk_group_use(child_grp, mw_group_child);

    /* 3. Register route with route-level middleware + business handler */
    csilk_handler_t route_hs[] = {mw_route, handler_target};
    csilk_group_add_handlers(child_grp, "GET", "/resource", route_hs, 2);

    /* 4. Match context */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/api/v1/resource");

    int matched = csilk_router_match_ctx(r, ctx);
    assert(matched == 1);
    assert(ctx->handler_count == 6); /* global1, global2, parent, child, route, target */

    /* Verify handler chain pointer is directly pointing to router's compiled chain (0 copy) */
    csilk_method_handler_t* mh = (csilk_method_handler_t*)ctx->current_handler;
    assert(mh != NULL);
    assert(ctx->handlers == mh->handlers);
    assert(mh->handler_count == 6);

    /* Execute chain */
    reset_trace();
    csilk_next(ctx);

    /* Verify onion execution trace: 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 105 -> 104 -> 103 -> 102 -> 101 */
    int expected_trace[] = {1, 2, 3, 4, 5, 6, 105, 104, 103, 102, 101};
    assert(g_trace_count == 11);
    for (int i = 0; i < 11; i++) {
        assert(g_trace_events[i] == expected_trace[i]);
    }
    assert(csilk_get_status(ctx) == CSILK_STATUS_OK);

    csilk_test_ctx_free(ctx);
    csilk_group_free(child_grp);
    csilk_group_free(parent_grp);
    csilk_server_free(s);
    csilk_router_free(r);
    printf("test_full_chain_order_and_zero_alloc: PASS\n");
}

/* Test 2: Short-circuiting */
static void
test_middleware_short_circuit(void)
{
    printf("Testing middleware short-circuiting...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r);

    csilk_server_use(s, mw_global_1);
    csilk_server_use(s, mw_short_circuit); /* Short-circuits here */
    csilk_server_use(s, mw_global_2);

    csilk_handler_t hs[] = {handler_target};
    csilk_router_add(r, "GET", "/test", hs, 1);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/test");

    assert(csilk_router_match_ctx(r, ctx) == 1);
    reset_trace();
    csilk_next(ctx);

    /* Expected: global1 in (1) -> short_circuit (99) -> global1 out (101) */
    assert(g_trace_count == 3);
    assert(g_trace_events[0] == 1);
    assert(g_trace_events[1] == 99);
    assert(g_trace_events[2] == 101);
    assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);

    csilk_test_ctx_free(ctx);
    csilk_server_free(s);
    csilk_router_free(r);
    printf("test_middleware_short_circuit: PASS\n");
}

/* Test 3: csilk_abort() */
static void
test_middleware_abort(void)
{
    printf("Testing middleware abort...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r);

    csilk_server_use(s, mw_global_1);
    csilk_server_use(s, mw_abort);
    csilk_server_use(s, mw_global_2);

    csilk_handler_t hs[] = {handler_target};
    csilk_router_add(r, "GET", "/abort", hs, 1);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/abort");

    assert(csilk_router_match_ctx(r, ctx) == 1);
    reset_trace();
    csilk_next(ctx);

    /* Expected: global1 (1) -> abort (88) -> global1 (101) */
    assert(g_trace_count == 3);
    assert(g_trace_events[0] == 1);
    assert(g_trace_events[1] == 88);
    assert(g_trace_events[2] == 101);

    csilk_test_ctx_free(ctx);
    csilk_server_free(s);
    csilk_router_free(r);
    printf("test_middleware_abort: PASS\n");
}

/* Test 4: Dynamic Global Middleware Recompilation */
static void
test_dynamic_global_middleware_recompile(void)
{
    printf("Testing dynamic global middleware compilation...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_server_t* s = csilk_server_new(r);

    /* 1. Register route BEFORE adding global middlewares */
    csilk_handler_t hs[] = {handler_target};
    csilk_router_add(r, "GET", "/dynamic", hs, 1);

    csilk_ctx_t* ctx1 = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx1, "GET", "/dynamic");
    assert(csilk_router_match_ctx(r, ctx1) == 1);
    assert(ctx1->handler_count == 1);
    csilk_test_ctx_free(ctx1);

    /* 2. Dynamically add global middlewares */
    csilk_server_use(s, mw_global_1);
    csilk_server_use(s, mw_global_2);

    /* 3. Match again: router should have compiled the new chain */
    csilk_ctx_t* ctx2 = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx2, "GET", "/dynamic");
    assert(csilk_router_match_ctx(r, ctx2) == 1);
    assert(ctx2->handler_count == 3); /* global1, global2, target */

    reset_trace();
    csilk_next(ctx2);
    int expected[] = {1, 2, 6, 102, 101};
    assert(g_trace_count == 5);
    for (int i = 0; i < 5; i++) {
        assert(g_trace_events[i] == expected[i]);
    }

    csilk_test_ctx_free(ctx2);
    csilk_server_free(s);
    csilk_router_free(r);
    printf("test_dynamic_global_middleware_recompile: PASS\n");
}

int
main(void)
{
    printf("=== Running Middleware Chain Compilation Tests ===\n\n");
    test_full_chain_order_and_zero_alloc();
    test_middleware_short_circuit();
    test_middleware_abort();
    test_dynamic_global_middleware_recompile();
    printf("\n=== All Middleware Chain Compilation Tests Passed! ===\n");
    return 0;
}
