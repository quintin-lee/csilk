#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/router_internal.h"
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
benchmark_segment_scan(void)
{
    enum { operations = 1000000 };
    const char*     path = "/api/v1/users/12345/profile/details";
    volatile size_t total_length = 0;
    uint64_t        start_ns = read_time_ns();
    uint64_t        start_cycles = read_cycles();
    for (int i = 0; i < operations; i++) {
        const char* cursor = path;
        size_t      length = 0;
        while (get_next_segment(&cursor, &length) != NULL) {
            total_length += length;
        }
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)total_length;
    print_result("router_segment_scan", operations, elapsed_ns, elapsed_cycles);
}

static void
benchmark_router_mode(int route_count, int use_simd, const char* pattern, const char* query)
{
    enum { lookups = 100000 };
    csilk_router_t* router = csilk_router_new();
    assert(router != NULL);

    csilk_handler_t handlers[] = {dummy_handler};
    char            route[128];
    char            path[128];
    for (int i = 0; i < route_count; i++) {
        snprintf(route, sizeof(route), pattern, i);
        assert(csilk_router_add(router, "GET", route, handlers, 1) == 0);
    }
    assert(csilk_router_compile(router, NULL, 0) == 0);
    csilk_server_t server;
    memset(&server, 0, sizeof(server));
    atomic_init(&server.runtime_config.enable_simd, use_simd);
    csilk_ctx_t* context = csilk_test_ctx_new();
    assert(context != NULL);
    context->server = &server;
    context->request.method = "GET";

    volatile uintptr_t sink = 0;
    uint64_t           start_ns = read_time_ns();
    uint64_t           start_cycles = read_cycles();
    for (int i = 0; i < lookups; i++) {
        int index = i % route_count;
        snprintf(path, sizeof(path), query, index, i);
        context->request.path = path;
        sink ^= (uintptr_t)csilk_router_match_ctx(router, context);
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)sink;

    char stage[64];
    snprintf(stage,
             sizeof(stage),
             "router_%s_%s_%d_routes",
             use_simd ? "simd" : "scalar",
             pattern[1] == ':' ? "param" : (strchr(pattern, '*') ? "wildcard" : "static"),
             route_count);
    print_result(stage, lookups, elapsed_ns, elapsed_cycles);
    context->request.path = NULL;
    csilk_test_ctx_free(context);
    csilk_router_free(router);
}

static void
benchmark_headers_scale(int header_count)
{
    enum { lookups = 200000 };
    csilk_ctx_t* context = csilk_test_ctx_new();
    assert(context != NULL);

    char name[64];
    char value[64];
    for (int i = 0; i < header_count; i++) {
        snprintf(name, sizeof(name), "X-Benchmark-%d", i);
        snprintf(value, sizeof(value), "value-%d", i);
        csilk_set_request_header(context, name, value);
    }

    volatile uintptr_t sink = 0;
    uint64_t           start_ns = read_time_ns();
    uint64_t           start_cycles = read_cycles();
    for (int i = 0; i < lookups; i++) {
        int index = i % header_count;
        snprintf(name, sizeof(name), "X-Benchmark-%d", index);
        sink ^= (uintptr_t)csilk_get_header(context, name);
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;
    (void)sink;

    char stage[64];
    snprintf(stage, sizeof(stage), "header_lookup_%d_headers", header_count);
    print_result(stage, lookups, elapsed_ns, elapsed_cycles);
    csilk_test_ctx_free(context);
}

static void
benchmark_stream_scale(int stream_count)
{
    enum { cycles = 2000 };
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    uint64_t start_ns = read_time_ns();
    uint64_t start_cycles = read_cycles();
    for (int i = 0; i < cycles; i++) {
        for (int stream = 0; stream < stream_count; stream++) {
            int32_t      id = (int32_t)(stream * 2 + 1);
            csilk_ctx_t* context = csilk_h2_get_or_create_stream(&client, id);
            assert(context != NULL);
            assert(csilk_h2_remove_stream(&client, id) == 0);
        }
    }
    uint64_t elapsed_cycles = read_cycles() - start_cycles;
    uint64_t elapsed_ns = read_time_ns() - start_ns;

    char stage[64];
    snprintf(stage, sizeof(stage), "h2_stream_create_recycle_%d", stream_count);
    print_result(stage, (uint64_t)stream_count * cycles, elapsed_ns, elapsed_cycles);
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
    benchmark_segment_scan();
    const char* patterns[] = {"/api/service%d/items/:id/detail",
                              "/api/service%d/items/*path",
                              "/api/service%d/items/detail"};
    const char* queries[] = {"/api/service%d/items/%d/detail",
                             "/api/service%d/items/%d/extra/detail",
                             "/api/service%d/items/detail"};
    const int   route_counts[] = {1, 10, 100, 1000, 10000};
    for (size_t kind = 0; kind < 3; kind++) {
        for (size_t count = 0; count < sizeof(route_counts) / sizeof(route_counts[0]); count++) {
            benchmark_router_mode(route_counts[count], 0, patterns[kind], queries[kind]);
            benchmark_router_mode(route_counts[count], 1, patterns[kind], queries[kind]);
        }
    }
    benchmark_headers_scale(5);
    benchmark_headers_scale(10);
    benchmark_headers_scale(20);
    benchmark_headers_scale(50);
    benchmark_headers_scale(100);
    benchmark_stream_scale(1);
    benchmark_stream_scale(10);
    benchmark_stream_scale(100);
    benchmark_stream_scale(1000);
    benchmark_arena();
    printf("=== Performance Model Complete ===\n");
    return EXIT_SUCCESS;
}
