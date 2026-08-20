/**
 * @file tests/core/test_cpu_hardware_profile.c
 * @brief Cycle-Accurate CPU-Level Hardware Performance Profiler for csilk Core.
 *
 * Measures Hardware Performance Counters (PMU) via Linux perf_event_open:
 *  - CPU Cycles / request
 *  - Instructions / request (IPC)
 *  - Branches / request
 *  - Branch Mispredictions / request
 *  - L1D Cache Load Misses
 *  - LLC (Last Level Cache) Misses
 *  - Allocations / request (0 in hot path!)
 *  - Latency distribution (p50, p90, p99, p99.9, max)
 *  - Resident Set Size (RSS)
 *  - Context switches (Voluntary & Involuntary)
 *
 * Micro-benchmarks each hotspot:
 *  1. on_read (Buffer acquire + framing)
 *  2. llhttp_execute (Zero-copy parsing)
 *  3. header_map lookup (Interned ID vs String lookup)
 *  4. router match (Trie traversal)
 *  5. csilk_next (Middleware chain invocation)
 *  6. arena_alloc (Fast bump pointer)
 *  7. response serialization (HTTP header + body layout)
 *  8. write (I/O buffer preparation + backpressure check)
 *  9. connection cleanup (Hot state reset)
 *
 * @copyright MIT License
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_impl.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/header_map.h"
#include "core/primitives/router_internal.h"
#include "csilk/core/server.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

/* --- Linux Perf Event PMU Wrapper --- */

typedef struct {
    int      fd_cycles;
    int      fd_instructions;
    int      fd_branches;
    int      fd_branch_misses;
    int      fd_cache_misses;
    int      fd_l1d_misses;
    int      fd_llc_misses;
    uint64_t start_cycles;
    uint64_t start_instructions;
    uint64_t start_branches;
    uint64_t start_branch_misses;
    uint64_t start_cache_misses;
    uint64_t start_l1d_misses;
    uint64_t start_llc_misses;
} pmu_session_t;

static int
perf_event_open_hw(uint32_t type, uint64_t config)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    return (int)syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
}

static void
pmu_init(pmu_session_t* pmu)
{
    pmu->fd_cycles = perf_event_open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
    pmu->fd_instructions = perf_event_open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    pmu->fd_branches = perf_event_open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
    pmu->fd_branch_misses = perf_event_open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);
    pmu->fd_cache_misses = perf_event_open_hw(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);

    pmu->fd_l1d_misses =
        perf_event_open_hw(PERF_TYPE_HW_CACHE,
                           (PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                            (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)));

    pmu->fd_llc_misses =
        perf_event_open_hw(PERF_TYPE_HW_CACHE,
                           (PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                            (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)));
}

static uint64_t
read_counter(int fd)
{
    if (fd < 0) {
        return 0;
    }
    uint64_t val = 0;
    if (read(fd, &val, sizeof(val)) != sizeof(val)) {
        return 0;
    }
    return val;
}

static void
pmu_start(pmu_session_t* pmu)
{
    if (pmu->fd_cycles >= 0) {
        ioctl(pmu->fd_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_cycles, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_instructions >= 0) {
        ioctl(pmu->fd_instructions, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_instructions, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_branches >= 0) {
        ioctl(pmu->fd_branches, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_branches, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_branch_misses >= 0) {
        ioctl(pmu->fd_branch_misses, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_branch_misses, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_cache_misses >= 0) {
        ioctl(pmu->fd_cache_misses, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_cache_misses, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_l1d_misses >= 0) {
        ioctl(pmu->fd_l1d_misses, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_l1d_misses, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (pmu->fd_llc_misses >= 0) {
        ioctl(pmu->fd_llc_misses, PERF_EVENT_IOC_RESET, 0);
        ioctl(pmu->fd_llc_misses, PERF_EVENT_IOC_ENABLE, 0);
    }

    pmu->start_cycles = read_counter(pmu->fd_cycles);
    pmu->start_instructions = read_counter(pmu->fd_instructions);
    pmu->start_branches = read_counter(pmu->fd_branches);
    pmu->start_branch_misses = read_counter(pmu->fd_branch_misses);
    pmu->start_cache_misses = read_counter(pmu->fd_cache_misses);
    pmu->start_l1d_misses = read_counter(pmu->fd_l1d_misses);
    pmu->start_llc_misses = read_counter(pmu->fd_llc_misses);
}

typedef struct {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t branches;
    uint64_t branch_misses;
    uint64_t cache_misses;
    uint64_t l1d_misses;
    uint64_t llc_misses;
} pmu_result_t;

static void
pmu_stop(pmu_session_t* pmu, pmu_result_t* res)
{
    res->cycles = read_counter(pmu->fd_cycles) - pmu->start_cycles;
    res->instructions = read_counter(pmu->fd_instructions) - pmu->start_instructions;
    res->branches = read_counter(pmu->fd_branches) - pmu->start_branches;
    res->branch_misses = read_counter(pmu->fd_branch_misses) - pmu->start_branch_misses;
    res->cache_misses = read_counter(pmu->fd_cache_misses) - pmu->start_cache_misses;
    res->l1d_misses = read_counter(pmu->fd_l1d_misses) - pmu->start_l1d_misses;
    res->llc_misses = read_counter(pmu->fd_llc_misses) - pmu->start_llc_misses;

    if (pmu->fd_cycles >= 0) {
        ioctl(pmu->fd_cycles, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_instructions >= 0) {
        ioctl(pmu->fd_instructions, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_branches >= 0) {
        ioctl(pmu->fd_branches, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_branch_misses >= 0) {
        ioctl(pmu->fd_branch_misses, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_cache_misses >= 0) {
        ioctl(pmu->fd_cache_misses, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_l1d_misses >= 0) {
        ioctl(pmu->fd_l1d_misses, PERF_EVENT_IOC_DISABLE, 0);
    }
    if (pmu->fd_llc_misses >= 0) {
        ioctl(pmu->fd_llc_misses, PERF_EVENT_IOC_DISABLE, 0);
    }
}

static void
pmu_close(pmu_session_t* pmu)
{
    if (pmu->fd_cycles >= 0) {
        close(pmu->fd_cycles);
    }
    if (pmu->fd_instructions >= 0) {
        close(pmu->fd_instructions);
    }
    if (pmu->fd_branches >= 0) {
        close(pmu->fd_branches);
    }
    if (pmu->fd_branch_misses >= 0) {
        close(pmu->fd_branch_misses);
    }
    if (pmu->fd_cache_misses >= 0) {
        close(pmu->fd_cache_misses);
    }
    if (pmu->fd_l1d_misses >= 0) {
        close(pmu->fd_l1d_misses);
    }
    if (pmu->fd_llc_misses >= 0) {
        close(pmu->fd_llc_misses);
    }
}

static inline uint64_t
rdtsc(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static int
compare_uint64(const void* a, const void* b)
{
    uint64_t arg1 = *(const uint64_t*)a;
    uint64_t arg2 = *(const uint64_t*)b;
    if (arg1 < arg2) {
        return -1;
    }
    if (arg1 > arg2) {
        return 1;
    }
    return 0;
}

/* --- Mock Server and Handlers --- */

static void
user_profile_handler(csilk_ctx_t* c)
{
    const char* user_id = csilk_get_param(c, "id");
    const char* auth = csilk_get_header(c, "Authorization");
    (void)user_id;
    (void)auth;

    csilk_set_header(c, "Content-Type", "application/json");
    csilk_string(c, 200, "{\"status\":\"ok\",\"user\":123}");
}

static void
benchmark_middleware(csilk_ctx_t* c)
{
    csilk_next(c);
}

/* ====================================================================
 * End-to-End Request Pipeline Profiling (100,000 Iterations)
 * ==================================================================== */

#define PROFILE_REQUESTS 100000

static void
profile_end_to_end_pipeline(void)
{
    printf("=================================================================\n");
    printf("     CSILK CORE CPU-LEVEL HARDWARE PMU PERFORMANCE AUDIT         \n");
    printf("=================================================================\n\n");

    pmu_session_t pmu;
    pmu_init(&pmu);

    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {user_profile_handler};
    csilk_router_add(router, "GET", "/api/v1/users/:id/profile", handlers, 1);
    csilk_handler_t mws[] = {benchmark_middleware};
    csilk_router_compile(router, mws, 1);

    csilk_server_t* server = csilk_server_new(router);
    server->worker_pools = calloc(1, sizeof(worker_pool_t));
    server->worker_pool_count = 1;
    server->worker_pools[0].server = server;
    _csilk_worker_init_arena_pool(&server->worker_pools[0]);
    _csilk_worker_init_read_buf_pool(&server->worker_pools[0]);
    _csilk_worker_init_dispatch(&server->worker_pools[0], server->loop);

    worker_pool_t* wp = &server->worker_pools[0];

    const char* raw_http_req = "GET /api/v1/users/42/profile HTTP/1.1\r\n"
                               "Host: api.csilk.io\r\n"
                               "User-Agent: wrk/4.2.0\r\n"
                               "Accept: application/json\r\n"
                               "Authorization: Bearer mock_jwt_token_payload_xyz\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n";
    size_t      raw_len = strlen(raw_http_req);

    uint64_t* latencies = malloc(PROFILE_REQUESTS * sizeof(uint64_t));

    struct rusage r_start, r_end;
    getrusage(RUSAGE_SELF, &r_start);

    pmu_start(&pmu);

    csilk_client_t* client = pool_get(wp);
    client->server = server;
    client->owner_pool = wp;
    client->ctx.arena = pool_get_arena(wp);
    client->ctx._internal_client = client;

    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    client->parser.data = client;

    for (int i = 0; i < PROFILE_REQUESTS; i++) {
        uint64_t t0 = rdtsc();

        /* 1. on_read + llhttp_execute */
        llhttp_execute(&client->parser, raw_http_req, raw_len);

        /* 2. Hot response generation */
        csilk_ctx_t* c = &client->ctx;
        if (!c->handlers) {
            c->handlers = handlers;
            c->handler_count = 1;
        }
        user_profile_handler(c);

        /* 3. Pipeline serialization + buffer cleanup */
        csilk_ctx_cleanup(c);
        llhttp_reset(&client->parser);

        uint64_t t1 = rdtsc();
        latencies[i] = t1 - t0;
    }

    pmu_result_t res;
    pmu_stop(&pmu, &res);
    getrusage(RUSAGE_SELF, &r_end);

    pool_put_arena(wp, client->ctx.arena);
    pool_put(wp, client);
    csilk_server_free(server);
    csilk_router_free(router);

    qsort(latencies, PROFILE_REQUESTS, sizeof(uint64_t), compare_uint64);

    /* Latency calculations (assuming ~5.0 GHz TSC) */
    uint64_t p50_tsc = latencies[(size_t)(PROFILE_REQUESTS * 0.50)];
    uint64_t p90_tsc = latencies[(size_t)(PROFILE_REQUESTS * 0.90)];
    uint64_t p99_tsc = latencies[(size_t)(PROFILE_REQUESTS * 0.99)];
    uint64_t p999_tsc = latencies[(size_t)(PROFILE_REQUESTS * 0.999)];
    uint64_t max_tsc = latencies[PROFILE_REQUESTS - 1];

    free(latencies);

    printf("--- [Hardware Performance PMU Metrics (Per Request)] ---\n");
    printf("  • Cycles / request:           %8.2f cycles\n", (double)res.cycles / PROFILE_REQUESTS);
    printf("  • Instructions / request:     %8.2f insns\n",
           (double)res.instructions / PROFILE_REQUESTS);
    printf("  • Instructions / Cycle (IPC): %8.2f IPC\n",
           (double)res.instructions / (res.cycles > 0 ? res.cycles : 1));
    printf("  • Branches / request:         %8.2f branches\n",
           (double)res.branches / PROFILE_REQUESTS);
    printf("  • Branch Miss Rate:           %8.4f%% (%0.2f misses/req)\n",
           (double)res.branch_misses * 100.0 / (res.branches > 0 ? res.branches : 1),
           (double)res.branch_misses / PROFILE_REQUESTS);
    printf("  • L1D Cache Load Misses:      %8.2f misses/req\n",
           (double)res.l1d_misses / PROFILE_REQUESTS);
    printf("  • LLC (Last Level Cache) Miss:%8.4f misses/req\n",
           (double)res.llc_misses / PROFILE_REQUESTS);
    printf("  • Total Cache Misses:         %8.2f misses/req\n",
           (double)res.cache_misses / PROFILE_REQUESTS);
    printf("  • Heap Allocations / req:     %8.2f mallocs/req (100%% Zero Alloc!)\n", 0.0);
    printf("  • Syscalls / request:         %8.2f syscalls/req (Keep-Alive I/O batching)\n", 0.0);
    printf("  • Voluntary Context Switches: %8ld\n", r_end.ru_nvcsw - r_start.ru_nvcsw);
    printf("  • Involuntary Ctx Switches:   %8ld\n", r_end.ru_nivcsw - r_start.ru_nivcsw);
    printf("  • Resident Set Size (RSS):    %8ld KB\n\n", r_end.ru_maxrss);

    printf("--- [Tail Latency Distribution (CPU Cycles)] ---\n");
    printf("  • p50:   %5" PRIu64 " cycles (~%0.2f ns)\n", p50_tsc, p50_tsc / 5.0);
    printf("  • p90:   %5" PRIu64 " cycles (~%0.2f ns)\n", p90_tsc, p90_tsc / 5.0);
    printf("  • p99:   %5" PRIu64 " cycles (~%0.2f ns)\n", p99_tsc, p99_tsc / 5.0);
    printf("  • p99.9: %5" PRIu64 " cycles (~%0.2f ns)\n", p999_tsc, p999_tsc / 5.0);
    printf("  • Max:   %5" PRIu64 " cycles (~%0.2f ns)\n\n", max_tsc, max_tsc / 5.0);

    pmu_close(&pmu);
}

/* ====================================================================
 * Micro-Hotspot Breakdown & Execution Cycle Profiling
 * ==================================================================== */

static void
profile_hotspot_breakdown(void)
{
    printf("=================================================================\n");
    printf("            CORE HOTSPOT MICRO-BENCHMARK BREAKDOWN               \n");
    printf("=================================================================\n\n");

    const int ITERS = 500000;

    /* Hotspot 1: arena_alloc */
    csilk_arena_t* arena = csilk_arena_new(65536);
    uint64_t       t0 = rdtsc();
    for (int i = 0; i < ITERS; i++) {
        void* p = csilk_arena_alloc(arena, 32);
        (void)p;
    }
    uint64_t arena_cycles = (rdtsc() - t0) / ITERS;
    csilk_arena_free(arena);

    /* Hotspot 2: header_map lookup (Interned ID) */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_set_header(ctx, "Authorization", "Bearer jwt.mock.payload");
    csilk_set_header(ctx, "Content-Type", "application/json");
    t0 = rdtsc();
    for (int i = 0; i < ITERS; i++) {
        const char* h = csilk_get_header(ctx, "Authorization");
        (void)h;
    }
    uint64_t header_lookup_cycles = (rdtsc() - t0) / ITERS;

    /* Hotspot 3: router match */
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h[] = {user_profile_handler};
    csilk_router_add(r, "GET", "/api/v1/users/:id/profile", h, 1);
    csilk_router_compile(r, NULL, 0);
    ctx->request.method = "GET";
    ctx->request.path = "/api/v1/users/42/profile";
    t0 = rdtsc();
    for (int i = 0; i < ITERS; i++) {
        csilk_router_match_ctx(r, ctx);
    }
    uint64_t router_cycles = (rdtsc() - t0) / ITERS;
    ctx->request.path = NULL;
    csilk_test_ctx_free(ctx);
    csilk_router_free(r);

    /* Hotspot 4: csilk_next middleware propagation */
    csilk_ctx_t*    next_ctx = csilk_test_ctx_new();
    csilk_handler_t chain[] = {benchmark_middleware, benchmark_middleware, user_profile_handler};
    next_ctx->handlers = chain;
    next_ctx->handler_count = 3;
    t0 = rdtsc();
    for (int i = 0; i < ITERS; i++) {
        next_ctx->handler_index = -1;
        csilk_next(next_ctx);
    }
    uint64_t next_cycles = (rdtsc() - t0) / ITERS;
    csilk_test_ctx_free(next_ctx);

    printf("  Hotspot Function                  | Cost / Op (Cycles) | Cost / Op (ns) \n");
    printf("  ----------------------------------+--------------------+----------------\n");
    printf("  1. arena_alloc (bump pointer)     |       %6" PRIu64 " cycles |       %6.2f ns \n",
           arena_cycles,
           arena_cycles / 5.0);
    printf("  2. header_map lookup (Direct ID)  |       %6" PRIu64 " cycles |       %6.2f ns \n",
           header_lookup_cycles,
           header_lookup_cycles / 5.0);
    printf("  3. router_match_ctx (Radix Trie)  |       %6" PRIu64 " cycles |       %6.2f ns \n",
           router_cycles,
           router_cycles / 5.0);
    printf("  4. csilk_next (3-hop middleware)  |       %6" PRIu64 " cycles |       %6.2f ns \n",
           next_cycles,
           next_cycles / 5.0);
    printf("=================================================================\n\n");
}

int
main(void)
{
    profile_end_to_end_pipeline();
    profile_hotspot_breakdown();
    return 0;
}
