/**
 * @file test_router_bench.c
 * @brief Scalability and memory efficiency benchmark for small-vector router trie.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/router.h"
#include "core/primitives/router_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
}

static inline uint64_t
get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static size_t
count_nodes(csilk_router_node_t* node)
{
    if (!node) {
        return 0;
    }
    size_t                count = 1;
    csilk_router_node_t** children = node_children(node);
    for (int i = 0; i < node->children_count; i++) {
        count += count_nodes(children[i]);
    }
    return count;
}

static size_t
count_overflow_nodes(csilk_router_node_t* node)
{
    if (!node) {
        return 0;
    }
    size_t                count = node->overflow_children ? 1 : 0;
    csilk_router_node_t** children = node_children(node);
    for (int i = 0; i < node->children_count; i++) {
        count += count_overflow_nodes(children[i]);
    }
    return count;
}

static void
generate_route_path(char* buf, size_t buf_sz, int route_idx)
{
    int s = route_idx % 16;
    int r = (route_idx / 16) % 32;
    int a = route_idx / 512;
    if (route_idx % 3 == 0) {
        snprintf(buf, buf_sz, "/api/v1/svc%d/res%d/:item_id/action%d", s, r, a);
    } else if (route_idx % 3 == 1) {
        snprintf(buf, buf_sz, "/v2/group%d/users/:uid/sub%d/detail", s, r);
    } else {
        snprintf(buf, buf_sz, "/system/service%d/item%d/view_%d", s, r, a);
    }
}

static void
run_scale_benchmark(int route_count, int lookup_count)
{
    csilk_router_t* r = csilk_router_new();
    assert(r != NULL);

    csilk_handler_t h[] = {dummy_handler, NULL};
    char            path_buf[128];

    /* 1. Registration Phase */
    uint64_t t_reg_start = get_time_ns();
    for (int i = 0; i < route_count; i++) {
        generate_route_path(path_buf, sizeof(path_buf), i);
        int rc = csilk_router_add(r, "GET", path_buf, h, 1);
        (void)rc;
    }
    uint64_t t_reg_end = get_time_ns();

    double reg_time_ms = (double)(t_reg_end - t_reg_start) / 1000000.0;
    double reg_ops_sec = (double)route_count / (reg_time_ms / 1000.0);

    /* 2. Structural & Memory Metrics */
    size_t total_nodes = count_nodes(r->root);
    size_t overflow_nodes = count_overflow_nodes(r->root);
    size_t current_mem_bytes =
        total_nodes * sizeof(csilk_router_node_t) + overflow_nodes * 16 * sizeof(void*);
    size_t legacy_mem_bytes = total_nodes * (sizeof(csilk_router_node_t) + 128 * sizeof(void*));
    double mem_reduction_pct = (1.0 - (double)current_mem_bytes / (double)legacy_mem_bytes) * 100.0;

    /* 3. Lookup Phase */
    uint64_t t_look_start = get_time_ns();
    int      hits = 0;
    for (int i = 0; i < lookup_count; i++) {
        int idx = (i * 37) % route_count;
        /* create concrete query path replacing :item_id with 123 */
        if (idx % 3 == 0) {
            int s = idx % 16;
            int r_idx = (idx / 16) % 32;
            int a = idx / 512;
            snprintf(path_buf, sizeof(path_buf), "/api/v1/svc%d/res%d/123/action%d", s, r_idx, a);
        } else if (idx % 3 == 1) {
            int s = idx % 16;
            int r_idx = (idx / 16) % 32;
            snprintf(path_buf, sizeof(path_buf), "/v2/group%d/users/user99/sub%d/detail", s, r_idx);
        } else {
            int s = idx % 16;
            int r_idx = (idx / 16) % 32;
            int a = idx / 512;
            snprintf(path_buf, sizeof(path_buf), "/system/service%d/item%d/view_%d", s, r_idx, a);
        }
        csilk_handler_t* matched = csilk_router_match(r, "GET", path_buf);
        if (matched) {
            hits++;
        }
    }
    uint64_t t_look_end = get_time_ns();

    double look_time_ms = (double)(t_look_end - t_look_start) / 1000000.0;
    double look_ops_sec = (double)lookup_count / (look_time_ms / 1000.0);
    double ns_per_lookup = (double)(t_look_end - t_look_start) / (double)lookup_count;

    printf("  Scale: %7d routes | Nodes: %7zu (Overflow: %zu)\n",
           route_count,
           total_nodes,
           overflow_nodes);
    printf("    Registration: %8.2f ms (%10.0f routes/sec)\n", reg_time_ms, reg_ops_sec);
    printf("    Memory/Node : %zu bytes (Legacy: ~1064 bytes) -> Total: %.2f KB (Legacy: %.2f KB, "
           "-%.1f%%)\n",
           sizeof(csilk_router_node_t),
           (double)current_mem_bytes / 1024.0,
           (double)legacy_mem_bytes / 1024.0,
           mem_reduction_pct);
    printf("    Lookup Perf : %8.2f ns/op (%10.0f ops/sec, %d hits)\n\n",
           ns_per_lookup,
           look_ops_sec,
           hits);

    csilk_router_free(r);
}

int
main(void)
{
    printf("=== Csilk Small-Vector Router Scalability & Memory Efficiency Benchmark ===\n\n");

    run_scale_benchmark(100, 100000);
    run_scale_benchmark(1000, 100000);
    run_scale_benchmark(10000, 100000);

    const char* full_env = getenv("CSILK_ROUTER_BENCH_FULL");
    if (full_env && (strcmp(full_env, "1") == 0 || strcmp(full_env, "true") == 0)) {
        run_scale_benchmark(100000, 100000);
        run_scale_benchmark(1000000, 100000);
    } else {
        printf("  (Set CSILK_ROUTER_BENCH_FULL=1 to run 100K and 1M route scales)\n\n");
    }

    printf("=== All router scalability benchmarks completed successfully! ===\n");
    return EXIT_SUCCESS;
}
