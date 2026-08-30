#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t
read_cycles(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static inline uint64_t
read_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
#endif

static inline uint64_t
read_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
}

static void
print_result(const char* stage, uint64_t operations, uint64_t elapsed_ns, uint64_t elapsed_cycles)
{
    double ns_per_operation = operations ? (double)elapsed_ns / (double)operations : 0.0;
    double cycles_per_operation = operations ? (double)elapsed_cycles / (double)operations : 0.0;
    printf("PERF stage=%s operations=%" PRIu64 " ns_per_op=%.2f cycles_per_op=%.2f "
           "elapsed_ns=%" PRIu64 "\n",
           stage,
           operations,
           ns_per_operation,
           cycles_per_operation,
           elapsed_ns);
}

static void
benchmark_router(void)
{
    enum { route_count = 1000, lookups = 200000 };
    csilk_router_t* router = csilk_router_new();
    assert(router != NULL);

    csilk_handler_t handlers[] = {dummy_handler};
    char            route[128];
    char            path[128];
    for (int i = 0; i < route_count; i++) {
        snprintf(route, sizeof(route), "/api/service%d/items/:id/detail", i);
        assert(csilk_router_add(router, "GET", route, handlers, 1) == 0);
    }
    assert(csilk_router_compile(router, NULL, 0) == 0);

    volatile uintptr_t sink = 0;
    uint64_t           start_ns = read_time_ns();
    uint64_t           start_cycles = read_cycles();
    for (int i = 0; i < lookups; i++) {
        int index = i % route_count;
        snprintf(path, sizeof(path), "/api/service%d/items/%d/detail", index, i);
        sink ^= (uintptr_t)csilk_router_match(router, "GET", path);
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)sink;

    print_result("router_match_1000_routes", lookups, elapsed_ns, elapsed_cycles);
    csilk_router_free(router);
}

static void
benchmark_headers(void)
{
    enum { lookups = 400000 };
    csilk_ctx_t* context = csilk_test_ctx_new();
    assert(context != NULL);
    csilk_set_request_header(context, "Host", "api.csilk.test");
    csilk_set_request_header(context, "Authorization", "Bearer benchmark");
    csilk_set_request_header(context, "Content-Type", "application/json");
    csilk_set_request_header(context, "X-Request-ID", "benchmark-request");
    csilk_set_request_header(context, "Accept", "application/json");

    volatile uintptr_t sink = 0;
    uint64_t           start_ns = read_time_ns();
    uint64_t           start_cycles = read_cycles();
    for (int i = 0; i < lookups; i++) {
        sink ^= (uintptr_t)csilk_get_header(context, "Authorization");
        sink ^= (uintptr_t)csilk_get_header_id(context, CSILK_HDR_CONTENT_TYPE);
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)sink;

    print_result("header_lookup_5_headers", lookups * 2, elapsed_ns, elapsed_cycles);
    csilk_test_ctx_free(context);
}

static void
benchmark_streams(void)
{
    enum { streams = 100, cycles = 10000 };
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    uint64_t start_ns = read_time_ns();
    uint64_t start_cycles = read_cycles();
    for (int i = 0; i < cycles; i++) {
        for (int stream = 0; stream < streams; stream++) {
            int32_t      id = (int32_t)(stream * 2 + 1);
            csilk_ctx_t* context = csilk_h2_get_or_create_stream(&client, id);
            assert(context != NULL);
            assert(csilk_h2_remove_stream(&client, id) == 0);
        }
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;

    print_result(
        "h2_stream_create_recycle_100", (uint64_t)streams * cycles, elapsed_ns, elapsed_cycles);
    csilk_h2_free_streams(&client);
}

static void
benchmark_arena(void)
{
    enum { allocations = 1000000 };
    csilk_arena_t* arena = csilk_arena_new(65536);
    assert(arena != NULL);

    volatile uintptr_t sink = 0;
    uint64_t           start_ns = read_time_ns();
    uint64_t           start_cycles = read_cycles();
    for (int i = 0; i < allocations; i++) {
        sink ^= (uintptr_t)csilk_arena_alloc(arena, 32);
        if ((i & 2047) == 2047) {
            csilk_arena_reset(arena);
        }
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)sink;

    print_result("arena_alloc_32_bytes", allocations, elapsed_ns, elapsed_cycles);
    csilk_arena_free(arena);
}

int
main(void)
{
    printf("=== CSilk Performance Model Baseline ===\n");
    benchmark_router();
    benchmark_headers();
    benchmark_streams();
    benchmark_arena();
    printf("=== Performance Model Complete ===\n");
    return EXIT_SUCCESS;
}
