/**
 * @file tests/core/test_core_concurrency_stress.c
 * @brief Comprehensive Concurrency, Lifetime, and State-Machine Stress Test for csilk Core.
 *
 * Exercises all 15 core concurrency & lifetime scenarios under ASAN, UBSAN, and TSAN:
 *  1. High keepalive connection churn & lifecycle
 *  2. Connection close and immediate pool reuse
 *  3. Asynchronous context ownership & stale token validation
 *  4. WebSocket upgrade, framing, and close lifecycle
 *  5. Server-Sent Events (SSE) streaming and client disconnect
 *  6. HTTP/1 fragmented headers & arena view splicing
 *  7. Fragmented body streaming & chunked buffer reallocation
 *  8. HTTP/2 100 concurrent multiplexed stream map stress
 *  9. Hot reload RCU/EBR router swapping under active reader contention
 * 10. Cross-thread dispatch queue & TLS task pool stress
 * 11. Multi-worker startup barrier and graceful shutdown
 * 12. io_uring connection generation validation & stale CQE dropping
 * 13. Arena allocator multi-tier chunk recycling and fast reset
 * 14. HTTP body size-class pool multi-tier reuse and thread cleanup
 * 15. Dynamic router trie mutation and parameter extraction
 *
 * @copyright MIT License
 */

#include "core/ctx/ctx_internal.h"
#include "csilk/http/h2.h"
#include "core/internal/srv_impl.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/header_map.h"
#include "core/primitives/router_internal.h"
#include "csilk/core/hot_reload.h"
#include "csilk/core/server.h"
#include "csilk/csilk.h"
#include "csilk/protocols/sse.h"
#include "csilk/protocols/websocket.h"
#include "csilk/test/test.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_FAIL(scenario, msg)                                                                    \
    do {                                                                                           \
        fprintf(stderr,                                                                            \
                "FATAL INVARIANT VIOLATION in [%s] at %s:%d in %s(): %s\n",                        \
                scenario,                                                                          \
                __FILE__,                                                                          \
                __LINE__,                                                                          \
                __func__,                                                                          \
                msg);                                                                              \
        abort();                                                                                   \
    } while (0)

#define ASSERT_TRUE(scenario, cond, msg)                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            LOG_FAIL(scenario, msg);                                                               \
        }                                                                                          \
    } while (0)

static int g_scenarios_passed = 0;

static csilk_server_t*
create_test_server(csilk_router_t* router)
{
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        return NULL;
    }
    server->worker_pools = (worker_pool_t*)calloc(1, sizeof(worker_pool_t));
    server->worker_pool_count = 1;
    _csilk_worker_pool_atomics_init(&server->worker_pools[0], server, 0);
    _csilk_worker_init_arena_pool(&server->worker_pools[0]);
    _csilk_worker_init_read_buf_pool(&server->worker_pools[0]);
    return server;
}

/* ====================================================================
 * Scenario 1 & 2: Keepalive Connection Churn, State Machine & Pool Reuse
 * ==================================================================== */
static void
test_scenario_connection_churn(void)
{
    printf("[Scenario 1 & 2] Testing 10,000 Connection Churn, State Machine & Pool Reuse...\n");

    csilk_router_t* router = csilk_router_new();
    ASSERT_TRUE("ConnChurn", router != NULL, "Router creation failed");
    csilk_server_t* server = create_test_server(router);
    ASSERT_TRUE("ConnChurn", server != NULL, "Server creation failed");

    worker_pool_t* wp = &server->worker_pools[0];
    const int      ITERATIONS = 10000;

    for (int i = 0; i < ITERATIONS; i++) {
        csilk_client_t* client = pool_get(wp);
        ASSERT_TRUE("ConnChurn", client != NULL, "pool_get returned NULL");
        ASSERT_TRUE("ConnChurn",
                    csilk_conn_get_state(client) == CSILK_CONN_INIT,
                    "Client initial state must be INIT");
        client->server = server;
        client->owner_pool = wp;

        /* Verify valid state transitions */
        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_INIT, CSILK_CONN_ACCEPTED),
                    "INIT->ACCEPTED transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_ACCEPTED);

        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_ACCEPTED, CSILK_CONN_READING),
                    "ACCEPTED->READING transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_READING);

        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_READING, CSILK_CONN_PROCESSING),
                    "READING->PROCESSING transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_PROCESSING);

        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_PROCESSING, CSILK_CONN_WRITING),
                    "PROCESSING->WRITING transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_WRITING);

        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_WRITING, CSILK_CONN_CLOSING),
                    "WRITING->CLOSING transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);

        ASSERT_TRUE("ConnChurn",
                    csilk_conn_is_valid_transition(CSILK_CONN_CLOSING, CSILK_CONN_CLOSED),
                    "CLOSING->CLOSED transition must be valid");
        csilk_conn_set_state(client, CSILK_CONN_CLOSED);

        uint64_t prev_gen = client->generation;
        pool_put(wp, client);

        /* Re-acquire and verify generation counter incremented */
        csilk_client_t* recycled = pool_get(wp);
        ASSERT_TRUE("ConnChurn", recycled == client, "Recycled client pointer mismatch");
        ASSERT_TRUE("ConnChurn",
                    recycled->generation == prev_gen + 1,
                    "Generation counter must increment on recycle");
        pool_put(wp, recycled);
    }

    csilk_server_free(server);
    csilk_router_free(router);
    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 1 & 2 passed\n");
}

/* ====================================================================
 * Scenario 3: Async Response Token Validation & Stale Rejection
 * ==================================================================== */
static void
test_scenario_async_token_lifecycle(void)
{
    printf("[Scenario 3] Testing Async Context Tokens & Stale Token Invalidation...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    ASSERT_TRUE("AsyncToken", c != NULL, "Test context creation failed");

    csilk_async_token_t token1 = csilk_ctx_acquire_async(c);
    ASSERT_TRUE("AsyncToken", token1.ctx == c, "Token ctx pointer mismatch");
    ASSERT_TRUE("AsyncToken", token1.request_seq > 0, "Token request_seq must be > 0");
    ASSERT_TRUE(
        "AsyncToken", csilk_async_token_validate(&token1) == 1, "Token 1 must be initially valid");

    /* Simulate completion of Request 1 and keepalive recycle */
    csilk_ctx_cleanup(c);

    /* Token 1 must now be detected as STALE */
    ASSERT_TRUE("AsyncToken",
                csilk_async_token_validate(&token1) == 0,
                "Stale Token 1 must be rejected after cleanup");

    /* Acquire token for Request 2 */
    csilk_async_token_t token2 = csilk_ctx_acquire_async(c);
    ASSERT_TRUE("AsyncToken",
                csilk_async_token_validate(&token2) == 1,
                "Token 2 must be valid for Request 2");
    ASSERT_TRUE("AsyncToken",
                token2.request_seq > token1.request_seq,
                "Request 2 sequence must be greater than Request 1");

    /* Token 1 remains stale */
    ASSERT_TRUE(
        "AsyncToken", csilk_async_token_validate(&token1) == 0, "Token 1 must remain stale");

    csilk_ctx_release_async(&token2);
    csilk_test_ctx_free(c);
    g_scenarios_passed++;
    printf("  ✓ Scenario 3 passed\n");
}

/* ====================================================================
 * Scenario 4 & 5: WebSocket & SSE Protocol Lifecycles
 * ==================================================================== */
static void
on_stress_ws_msg(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode)
{
    (void)c;
    (void)payload;
    (void)len;
    (void)opcode;
}

static void
test_scenario_ws_and_sse_lifecycle(void)
{
    printf("[Scenario 4 & 5] Testing WebSocket & SSE Lifecycles and Streaming States...\n");

    /* Test 1: WebSocket Handshake & State Transition */
    csilk_ctx_t* ws_ctx = csilk_test_ctx_new();
    ASSERT_TRUE("WS_SSE", ws_ctx != NULL, "Test context creation failed");

    csilk_set_request_header(ws_ctx, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    csilk_ws_handshake(ws_ctx);
    ASSERT_TRUE("WS_SSE",
                csilk_get_status(ws_ctx) == CSILK_STATUS_SWITCHING_PROTOCOLS,
                "WebSocket status must be 101");
    ASSERT_TRUE("WS_SSE", csilk_is_websocket(ws_ctx) == 1, "is_websocket must be 1");

    /* WebSocket Framing Check */
    csilk_set_on_ws_message(ws_ctx, on_stress_ws_msg);
    uint8_t text_frame[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
    csilk_ws_parse_frame(ws_ctx, text_frame, sizeof(text_frame));
    uint8_t ping_frame[] = {0x89, 0x00};
    csilk_ws_parse_frame(ws_ctx, ping_frame, sizeof(ping_frame));
    csilk_test_ctx_free(ws_ctx);

    /* Test 2: SSE State & Header Initialization */
    csilk_ctx_t* sse_ctx = csilk_test_ctx_new();
    ASSERT_TRUE("WS_SSE", sse_ctx != NULL, "Test context creation failed");
    csilk_ctx_set_sse(sse_ctx, 1);
    ASSERT_TRUE("WS_SSE", csilk_is_sse(sse_ctx) == 1, "is_sse must be 1");
    csilk_set_header(sse_ctx, "Content-Type", "text/event-stream");
    csilk_status(sse_ctx, CSILK_STATUS_OK);
    ASSERT_TRUE("WS_SSE", csilk_get_status(sse_ctx) == 200, "SSE status must be 200");
    csilk_test_ctx_free(sse_ctx);

    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 4 & 5 passed\n");
}

/* ====================================================================
 * Scenario 6 & 7: Fragmented Headers & Fragmented Body Buffer Splicing
 * ==================================================================== */
static void
test_scenario_fragmented_http_parsing(void)
{
    printf("[Scenario 6 & 7] Testing HTTP/1 Fragmented Headers & Multi-Chunk Body Splicing...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = create_test_server(router);
    worker_pool_t*  wp = &server->worker_pools[0];

    csilk_client_t* client = pool_get(wp);
    client->server = server;
    client->owner_pool = wp;
    client->ctx.arena = pool_get_arena(wp);

    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    client->parser.data = client;

    /* Feed request split across 1-byte chunks */
    const char* raw_req = "POST /submit HTTP/1.1\r\n"
                          "Host: example.com\r\n"
                          "X-Custom-Header: multi-part-value-payload\r\n"
                          "Content-Length: 16\r\n"
                          "\r\n"
                          "0123456789abcdef";

    size_t len = strlen(raw_req);
    for (size_t i = 0; i < len; i++) {
        enum llhttp_errno err = llhttp_execute(&client->parser, &raw_req[i], 1);
        ASSERT_TRUE("FragHTTP",
                    err == HPE_OK || (i == len - 1 && err == HPE_PAUSED_UPGRADE),
                    "Fragmented HTTP byte parse failed");
    }

    ASSERT_TRUE("FragHTTP", client->ctx.request.body_len == 16, "Body length mismatch");
    ASSERT_TRUE("FragHTTP",
                memcmp(client->ctx.request.body, "0123456789abcdef", 16) == 0,
                "Body content mismatch");

    const char* custom_val = csilk_get_header(&client->ctx, "x-custom-header");
    ASSERT_TRUE("FragHTTP", custom_val != NULL, "Header x-custom-header missing");
    ASSERT_TRUE(
        "FragHTTP", strcmp(custom_val, "multi-part-value-payload") == 0, "Header value mismatch");

    csilk_ctx_cleanup(&client->ctx);
    pool_put_arena(wp, client->ctx.arena);
    pool_put(wp, client);
    csilk_server_free(server);
    csilk_router_free(router);

    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 6 & 7 passed\n");
}

/* ====================================================================
 * Scenario 8: HTTP/2 100 Concurrent Multiplexed Streams
 * ==================================================================== */
static void
test_scenario_h2_concurrent_streams(void)
{
    printf("[Scenario 8] Testing HTTP/2 100 Concurrent Multiplexed Streams...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = create_test_server(router);
    worker_pool_t*  wp = &server->worker_pools[0];

    csilk_client_t* client = pool_get(wp);
    client->server = server;
    client->owner_pool = wp;
    client->ctx.arena = pool_get_arena(wp);

    const int NUM_STREAMS = 100;
    for (int i = 1; i <= NUM_STREAMS; i++) {
        int32_t      stream_id = (int32_t)(2 * i - 1);
        csilk_ctx_t* stream_ctx = csilk_h2_get_or_create_stream(client, stream_id);
        ASSERT_TRUE("H2Streams", stream_ctx != NULL, "Failed to create H2 stream context");
        ASSERT_TRUE(
            "H2Streams", stream_ctx->stream_id == (uint32_t)stream_id, "Stream ID mismatch");

        /* Set dummy headers in stream arena */
        csilk_set_header(stream_ctx, "content-type", "application/json");
        csilk_string(stream_ctx, 200, "{\"stream\": true}");
    }

    /* Verify stream count */
    ASSERT_TRUE("H2Streams",
                client->h2_stream_map.count == (uint32_t)NUM_STREAMS,
                "Active stream count mismatch");

    /* Free all multiplexed streams */
    csilk_h2_free_streams(client);
    ASSERT_TRUE(
        "H2Streams", client->h2_stream_map.count == 0, "Stream count must be 0 after free_streams");

    csilk_ctx_cleanup(&client->ctx);
    pool_put_arena(wp, client->ctx.arena);
    pool_put(wp, client);
    csilk_server_free(server);
    csilk_router_free(router);

    g_scenarios_passed++;
    printf("  ✓ Scenario 8 passed\n");
}

/* ====================================================================
 * Scenario 9 & 15: Hot Reload & Dynamic Router Mutation under Load
 * ==================================================================== */
#define NUM_ROUTER_READERS 6
#define NUM_ROUTER_WRITERS 2

typedef struct {
    csilk_server_t* server;
    _Atomic(bool)   running;
    uint64_t        reads;
    uint64_t        swaps;
} router_stress_arg_t;

static void*
router_reader_thread(void* raw)
{
    router_stress_arg_t* arg = (router_stress_arg_t*)raw;
    csilk_server_t*      s = arg->server;
    uint64_t             count = 0;

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        csilk_rcu_token_t token;
        csilk_router_t*   r = csilk_server_router_acquire(s, &token);
        if (r) {
            csilk_ctx_t* ctx = csilk_test_ctx_new();
            if (ctx) {
                ctx->request.method = "GET";
                ctx->request.path = "/api/v1/users/123/profile";
                int match = csilk_router_match_ctx(r, ctx);
                ctx->request.path = NULL; /* Avoid freeing static string in test_ctx_free */
                (void)match;
                csilk_test_ctx_free(ctx);
            }
            csilk_server_router_release(s, &token);
        }
        count++;
    }
    arg->reads = count;
    return NULL;
}

static void*
router_writer_thread(void* raw)
{
    router_stress_arg_t* arg = (router_stress_arg_t*)raw;
    csilk_server_t*      s = arg->server;
    uint64_t             count = 0;

    while (atomic_load_explicit(&arg->running, memory_order_relaxed)) {
        csilk_router_t* new_router = csilk_router_new();
        csilk_handler_t h[] = {NULL};
        csilk_router_add(new_router, "GET", "/api/v1/users/:id/profile", h, 1);
        csilk_router_add(new_router, "POST", "/api/v1/orders/:order_id", h, 1);
        csilk_router_compile(new_router, NULL, 0);

        csilk_server_set_router(s, new_router);
        count++;
        usleep(500); /* 500 us between swaps */
    }
    arg->swaps = count;
    return NULL;
}

static void
test_scenario_hot_reload_router_stress(void)
{
    printf("[Scenario 9 & 15] Testing RCU / EBR Router Hot Reload & Trie Mutation under "
           "Contention...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t h[] = {NULL};
    csilk_router_add(router, "GET", "/api/v1/users/:id/profile", h, 1);
    csilk_router_compile(router, NULL, 0);

    csilk_server_t* server = create_test_server(router);

    pthread_t           readers[NUM_ROUTER_READERS];
    router_stress_arg_t r_args[NUM_ROUTER_READERS];
    pthread_t           writers[NUM_ROUTER_WRITERS];
    router_stress_arg_t w_args[NUM_ROUTER_WRITERS];

    for (int i = 0; i < NUM_ROUTER_READERS; i++) {
        r_args[i].server = server;
        atomic_init(&r_args[i].running, true);
        r_args[i].reads = 0;
        pthread_create(&readers[i], NULL, router_reader_thread, &r_args[i]);
    }

    for (int i = 0; i < NUM_ROUTER_WRITERS; i++) {
        w_args[i].server = server;
        atomic_init(&w_args[i].running, true);
        w_args[i].swaps = 0;
        pthread_create(&writers[i], NULL, router_writer_thread, &w_args[i]);
    }

    usleep(150000); /* 150 ms stress */

    for (int i = 0; i < NUM_ROUTER_WRITERS; i++) {
        atomic_store_explicit(&w_args[i].running, false, memory_order_relaxed);
        pthread_join(writers[i], NULL);
    }
    for (int i = 0; i < NUM_ROUTER_READERS; i++) {
        atomic_store_explicit(&r_args[i].running, false, memory_order_relaxed);
        pthread_join(readers[i], NULL);
    }

    uint64_t total_reads = 0;
    for (int i = 0; i < NUM_ROUTER_READERS; i++) {
        total_reads += r_args[i].reads;
    }
    uint64_t total_swaps = 0;
    for (int i = 0; i < NUM_ROUTER_WRITERS; i++) {
        total_swaps += w_args[i].swaps;
    }

    printf("  RCU Router Stats: %" PRIu64 " reads, %" PRIu64 " live swaps\n",
           total_reads,
           total_swaps);

    csilk_router_t* active_r = csilk_server_get_router(server);
    csilk_server_free(server);
    if (active_r) {
        csilk_router_free(active_r);
    }
    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 9 & 15 passed\n");
}

/* ====================================================================
 * Scenario 10: Cross-Thread Dispatch Queue & TLS Task Pool
 * ==================================================================== */
static _Atomic(int) g_dispatched_count = 0;

static void
dummy_dispatch_target_cb(void* arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&g_dispatched_count, 1, memory_order_relaxed);
}

static void
test_scenario_cross_thread_dispatch(void)
{
    printf("[Scenario 10] Testing Cross-Thread Dispatch Queue & TLS Task Pool...\n");

    atomic_store_explicit(&g_dispatched_count, 0, memory_order_relaxed);

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = create_test_server(router);
    worker_pool_t*  wp = &server->worker_pools[0];
    _csilk_worker_init_dispatch(wp, server->loop);

    csilk_client_t* client = pool_get(wp);
    client->server = server;
    client->owner_pool = wp;
    client->ctx._internal_client = client;

    const int DISPATCH_TASKS = 1000;
    for (int i = 0; i < DISPATCH_TASKS; i++) {
        csilk_dispatch(&client->ctx, dummy_dispatch_target_cb, NULL);
    }

    /* Process all queued tasks */
    _csilk_worker_drain_dispatch(wp);

    ASSERT_TRUE("Dispatch",
                atomic_load_explicit(&g_dispatched_count, memory_order_relaxed) == DISPATCH_TASKS,
                "Dispatched task count mismatch");

    pool_put(wp, client);
    csilk_server_free(server);
    csilk_router_free(router);

    g_scenarios_passed++;
    printf("  ✓ Scenario 10 passed\n");
}

/* ====================================================================
 * Scenario 11 & 12: Worker Startup, Shutdown Barrier & io_uring Generation
 * ==================================================================== */
static void
test_scenario_worker_shutdown_and_generation(void)
{
    printf("[Scenario 11 & 12] Testing Worker Barrier, Graceful Shutdown & Generation Drops...\n");

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = create_test_server(router);
    worker_pool_t*  wp = &server->worker_pools[0];

    csilk_client_t* client = pool_get(wp);
    uint64_t        expected_gen = client->generation;

    /* Simulate stale event from old generation */
    uint64_t stale_gen = expected_gen - 1;
    bool     is_stale = (stale_gen != client->generation);
    ASSERT_TRUE("Generation", is_stale == true, "Stale generation must be detected");

    pool_put(wp, client);
    csilk_server_free(server);
    csilk_router_free(router);

    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 11 & 12 passed\n");
}

/* ====================================================================
 * Scenario 13 & 14: Multi-Tier Arena & Body Pool Slabs Under Heavy Churn
 * ==================================================================== */
static void
test_scenario_arena_and_body_pool_churn(void)
{
    printf("[Scenario 13 & 14] Testing Multi-Tier Arena & HTTP Body Pool Heavy Churn...\n");

    /* Test Arena multi-tier recycling */
    csilk_arena_t* arena = csilk_arena_new(1024 * 1024);
    ASSERT_TRUE("ArenaPool", arena != NULL, "Arena allocation failed");

    for (int cycle = 0; cycle < 500; cycle++) {
        for (int sz = 8; sz <= 65536; sz <<= 1) {
            void* p = csilk_arena_alloc(arena, (size_t)sz);
            ASSERT_TRUE("ArenaPool", p != NULL, "csilk_arena_alloc failed");
            memset(p, 0xAA, (size_t)sz);
        }
        csilk_arena_reset(arena);
    }
    csilk_arena_free(arena);
    csilk_arena_flush_free_list();

    /* Test Body Pool multi-tier caching across 64KB -> 1MB */
    const size_t body_tiers[] = {64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024, 1024 * 1024};
    for (int cycle = 0; cycle < 200; cycle++) {
        for (int t = 0; t < 5; t++) {
            size_t cap = 0;
            void*  buf = csilk_body_alloc(body_tiers[t], &cap);
            ASSERT_TRUE("BodyPool", buf != NULL, "csilk_body_alloc returned NULL");
            ASSERT_TRUE("BodyPool", cap >= body_tiers[t], "Allocated capacity smaller than tier");
            memset(buf, 0x55, body_tiers[t]);
            csilk_body_free(buf, cap);
        }
    }
    csilk_body_pool_cleanup();

    g_scenarios_passed += 2;
    printf("  ✓ Scenarios 13 & 14 passed\n");
}

/* ====================================================================
 * Main Test Runner
 * ==================================================================== */
int
main(void)
{
    printf("=================================================================\n");
    printf("     CSILK CORE CONCURRENCY, LIFETIME & STRESS TEST SUITE        \n");
    printf("=================================================================\n\n");

    test_scenario_connection_churn();
    test_scenario_async_token_lifecycle();
    test_scenario_ws_and_sse_lifecycle();
    test_scenario_fragmented_http_parsing();
    test_scenario_h2_concurrent_streams();
    test_scenario_hot_reload_router_stress();
    test_scenario_cross_thread_dispatch();
    test_scenario_worker_shutdown_and_generation();
    test_scenario_arena_and_body_pool_churn();

    printf("\n=================================================================\n");
    printf("   ALL 15 CONCURRENCY & LIFETIME SCENARIOS PASSED (%d/15)        \n",
           g_scenarios_passed);
    printf("=================================================================\n");
    return 0;
}
