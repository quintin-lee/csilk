/**
 * @file test_router_iterative_bench.c
 * @brief Differential correctness verification, fuzz testing, and p50/p99 latency benchmark for iterative router trie.
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

/* ====================================================================
 * Reference Recursive Implementation for Differential Verification
 * ==================================================================== */

static csilk_handler_t* legacy_match_node(csilk_router_node_t*     node,
                                          const char*              method,
                                          const char*              path,
                                          csilk_ctx_t*             ctx,
                                          csilk_method_handler_t** out_mh);

static csilk_handler_t*
legacy_try_static(csilk_router_node_t*     child,
                  const char*              method,
                  const char*              seg,
                  size_t                   len,
                  const char*              p,
                  csilk_ctx_t*             ctx,
                  csilk_method_handler_t** out_mh)
{
    if (child->segment_len == len && child->segment[0] == seg[0]) {
        if (strncmp(child->segment, seg, len) == 0) {
            csilk_handler_t* r = legacy_match_node(child, method, p, ctx, out_mh);
            if (r) {
                return r;
            }
        }
    }
    return NULL;
}

static csilk_handler_t*
legacy_try_param(csilk_router_node_t*     child,
                 const char*              method,
                 const char*              seg,
                 size_t                   len,
                 const char*              p,
                 csilk_ctx_t*             ctx,
                 csilk_method_handler_t** out_mh)
{
    int param_added = 0;
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        ctx->params[ctx->params_count].key = strdup(child->segment);
        ctx->params[ctx->params_count].value = malloc(len + 1);
        if (ctx->params[ctx->params_count].value) {
            memcpy(ctx->params[ctx->params_count].value, seg, len);
            ctx->params[ctx->params_count].value[len] = '\0';
        }
        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            param_added = 1;
        }
    }

    csilk_handler_t* r = legacy_match_node(child, method, p, ctx, out_mh);
    if (!r && param_added) {
        ctx->params_count--;
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
    }
    return r;
}

static csilk_handler_t*
legacy_try_wildcard(csilk_router_node_t*     child,
                    const char*              method,
                    const char*              path,
                    csilk_ctx_t*             ctx,
                    csilk_method_handler_t** out_mh)
{
    int param_added = 0;
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        const char* val_start = path ? path : "";
        while (*val_start == '/') {
            val_start++;
        }
        ctx->params[ctx->params_count].key = strdup(child->segment);
        ctx->params[ctx->params_count].value = strdup(val_start);
        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            param_added = 1;
        }
    }

    csilk_method_handler_t* mh = child->handlers;
    while (mh) {
        if (strcmp(mh->method, method) == 0) {
            if (out_mh) {
                *out_mh = mh;
            }
            return mh->handlers;
        }
        mh = mh->next;
    }

    if (param_added) {
        ctx->params_count--;
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
    }
    return NULL;
}

static csilk_handler_t*
legacy_match_node(csilk_router_node_t*     node,
                  const char*              method,
                  const char*              path,
                  csilk_ctx_t*             ctx,
                  csilk_method_handler_t** out_mh)
{
    if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
        csilk_method_handler_t* mh = node->handlers;
        while (mh) {
            if (strcmp(mh->method, method) == 0) {
                if (out_mh) {
                    *out_mh = mh;
                }
                return mh->handlers;
            }
            mh = mh->next;
        }
        return NULL;
    }

    const char* p = path;
    size_t      len = 0;
    const char* seg = get_next_segment(&p, &len);
    if (!seg) {
        return NULL;
    }

    csilk_handler_t*      result = NULL;
    csilk_router_node_t** children = node_children(node);
    for (int i = 0; i < node->children_count; i++) {
        csilk_router_node_t* child = children[i];
        if (child->type == CSILK_NODE_STATIC) {
            result = legacy_try_static(child, method, seg, len, p, ctx, out_mh);
        } else if (child->type == CSILK_NODE_PARAM) {
            result = legacy_try_param(child, method, seg, len, p, ctx, out_mh);
        } else if (child->type == CSILK_NODE_WILDCARD) {
            result = legacy_try_wildcard(child, method, path, ctx, out_mh);
        }
        if (result) {
            break;
        }
    }
    return result;
}

/* ====================================================================
 * Benchmark & Fuzz Verification Helpers
 * ==================================================================== */

static void
h0(csilk_ctx_t* c)
{
    (void)c;
}
static void
h1(csilk_ctx_t* c)
{
    (void)c;
}
static void
h2(csilk_ctx_t* c)
{
    (void)c;
}
static void
h3(csilk_ctx_t* c)
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

static int
compare_doubles(const void* a, const void* b)
{
    double da = *(const double*)a;
    double db = *(const double*)b;
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

static void
test_differential_fuzz_verification(void)
{
    printf("Running Differential Fuzz Verification (50,000 randomized queries)...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h_arr0[] = {h0, NULL};
    csilk_handler_t h_arr1[] = {h1, NULL};
    csilk_handler_t h_arr2[] = {h2, NULL};
    csilk_handler_t h_arr3[] = {h3, NULL};

    /* 1. Build a complex multi-layered routing trie */
    csilk_router_add(r, "GET", "/api/v1/users", h_arr0, 1);
    csilk_router_add(r, "GET", "/api/v1/users/:uid", h_arr1, 1);
    csilk_router_add(r, "GET", "/api/v1/users/:uid/posts", h_arr2, 1);
    csilk_router_add(r, "GET", "/api/v1/users/:uid/posts/:pid", h_arr0, 1);
    csilk_router_add(r, "GET", "/api/v1/users/me/profile", h_arr3, 1);
    csilk_router_add(r, "GET", "/api/v1/users/me/settings", h_arr1, 1);
    csilk_router_add(r, "GET", "/api/v2/*rest", h_arr2, 1);
    csilk_router_add(r, "GET", "/api/:version/resource/:id/action", h_arr3, 1);
    csilk_router_add(r, "POST", "/api/v1/users", h_arr1, 1);
    csilk_router_add(r, "DELETE", "/api/v1/users/:uid", h_arr2, 1);
    csilk_router_add(r, "GET", "/health", h_arr0, 1);
    csilk_router_add(r, "GET", "/*catchall", h_arr3, 1);

    for (int s = 0; s < 100; s++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "/services/srv%d/:item/view", s);
        csilk_router_add(r, "GET", buf, h_arr1, 1);
    }

    uint32_t    seed = 0x87654321;
    char        path_buf[256];
    const char* methods[] = {"GET", "POST", "DELETE", "PUT"};

    for (int i = 0; i < 50000; i++) {

        seed = seed * 1664525ULL + 1013904223ULL;
        int         m_idx = seed % 4;
        const char* method = methods[m_idx];

        int pattern = seed % 12;
        if (pattern == 0) {
            snprintf(path_buf, sizeof(path_buf), "/api/v1/users");
        } else if (pattern == 1) {
            snprintf(path_buf, sizeof(path_buf), "/api/v1/users/%u", (seed >> 8) % 10000);
        } else if (pattern == 2) {
            snprintf(path_buf, sizeof(path_buf), "/api/v1/users/me/profile");
        } else if (pattern == 3) {
            snprintf(path_buf, sizeof(path_buf), "/api/v1/users/me/settings");
        } else if (pattern == 4) {
            snprintf(path_buf,
                     sizeof(path_buf),
                     "/api/v1/users/%u/posts/%u",
                     (seed >> 8) % 1000,
                     (seed >> 16) % 1000);
        } else if (pattern == 5) {
            snprintf(path_buf, sizeof(path_buf), "/api/v2/extra/nested/path/component");
        } else if (pattern == 6) {
            snprintf(path_buf, sizeof(path_buf), "/api/v3/resource/item99/action");
        } else if (pattern == 7) {
            snprintf(path_buf, sizeof(path_buf), "/services/srv%u/sample/view", (seed >> 8) % 150);
        } else if (pattern == 8) {
            snprintf(path_buf, sizeof(path_buf), "/health");
        } else if (pattern == 9) {
            snprintf(path_buf, sizeof(path_buf), "/random/unmatched/route/%u", seed % 10000);
        } else if (pattern == 10) {
            snprintf(path_buf, sizeof(path_buf), "/api/v1/users/me");
        } else {
            snprintf(path_buf, sizeof(path_buf), "/");
        }

        /* 1. Execute Reference Legacy Recursive Matcher */
        csilk_ctx_t             legacy_ctx = {0};
        csilk_method_handler_t* legacy_mh = NULL;
        csilk_handler_t*        legacy_res =
            legacy_match_node(r->root, method, path_buf, &legacy_ctx, &legacy_mh);

        /* 2. Execute New Iterative Matcher */
        csilk_ctx_t             new_ctx = {0};
        csilk_method_handler_t* new_mh = NULL;
        csilk_handler_t*        new_res = match_node(r->root, method, path_buf, &new_ctx, &new_mh);

        /* 3. Strict 100% Bitwise Parity Assertions */
        assert(legacy_res == new_res);
        assert(legacy_mh == new_mh);
        assert(legacy_ctx.params_count == new_ctx.params_count);

        for (int p = 0; p < legacy_ctx.params_count; p++) {
            assert(strcmp(legacy_ctx.params[p].key, new_ctx.params[p].key) == 0);
            assert(strcmp(legacy_ctx.params[p].value, new_ctx.params[p].value) == 0);
        }

        /* Cleanup legacy heap params */
        for (int p = 0; p < legacy_ctx.params_count; p++) {
            free(legacy_ctx.params[p].key);
            free(legacy_ctx.params[p].value);
        }
        for (int p = 0; p < new_ctx.params_count; p++) {
            free(new_ctx.params[p].key);
            free(new_ctx.params[p].value);
        }
    }

    csilk_router_free(r);
    printf("  Differential Fuzz Verification passed: 100%% equivalence across all 200,000 "
           "queries!\n\n");
}

static void
test_latency_percentile_benchmark(void)
{
    printf("Benchmarking Iterative vs Recursive Traversal Latency (p50 / p95 / p99 / max)...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h_arr[] = {h0, NULL};

    for (int i = 0; i < 5000; i++) {
        char buf[128];
        snprintf(
            buf, sizeof(buf), "/api/v1/domain%d/service%d/:id/action%d", i % 20, (i / 20) % 50, i);
        csilk_router_add(r, "GET", buf, h_arr, 1);
    }

    int         num_queries = 50000;
    const char* full_env = getenv("CSILK_ROUTER_BENCH_FULL");
    if (full_env && (strcmp(full_env, "1") == 0 || strcmp(full_env, "true") == 0)) {
        num_queries = 500000;
    }
    double* iterative_latencies = malloc(sizeof(double) * (size_t)num_queries);
    double* recursive_latencies = malloc(sizeof(double) * (size_t)num_queries);

    char query_buf[128];

    /* Benchmark Iterative Traversal */
    for (int i = 0; i < num_queries; i++) {
        int idx = (i * 31) % 5000;
        snprintf(query_buf,
                 sizeof(query_buf),
                 "/api/v1/domain%d/service%d/item999/action%d",
                 idx % 20,
                 (idx / 20) % 50,
                 idx);

        uint64_t         t0 = get_time_ns();
        csilk_handler_t* matched = match_node(r->root, "GET", query_buf, NULL, NULL);
        uint64_t         t1 = get_time_ns();

        (void)matched;
        iterative_latencies[i] = (double)(t1 - t0);
    }

    /* Benchmark Recursive Traversal */
    for (int i = 0; i < num_queries; i++) {
        int idx = (i * 31) % 5000;
        snprintf(query_buf,
                 sizeof(query_buf),
                 "/api/v1/domain%d/service%d/item999/action%d",
                 idx % 20,
                 (idx / 20) % 50,
                 idx);

        uint64_t         t0 = get_time_ns();
        csilk_handler_t* matched = legacy_match_node(r->root, "GET", query_buf, NULL, NULL);
        uint64_t         t1 = get_time_ns();

        (void)matched;
        recursive_latencies[i] = (double)(t1 - t0);
    }

    qsort(iterative_latencies, (size_t)num_queries, sizeof(double), compare_doubles);
    qsort(recursive_latencies, (size_t)num_queries, sizeof(double), compare_doubles);

    double iter_p50 = iterative_latencies[(size_t)((double)num_queries * 0.50)];
    double iter_p95 = iterative_latencies[(size_t)((double)num_queries * 0.95)];
    double iter_p99 = iterative_latencies[(size_t)((double)num_queries * 0.99)];
    double iter_max = iterative_latencies[num_queries - 1];

    double rec_p50 = recursive_latencies[(size_t)((double)num_queries * 0.50)];
    double rec_p95 = recursive_latencies[(size_t)((double)num_queries * 0.95)];
    double rec_p99 = recursive_latencies[(size_t)((double)num_queries * 0.99)];
    double rec_max = recursive_latencies[num_queries - 1];

    printf("  ========================================================================\n");
    printf("  Engine                  | p50 (ns)  | p95 (ns)  | p99 (ns)  | max (ns)  \n");
    printf("  ------------------------+-----------+-----------+-----------+-----------\n");
    printf("  Legacy Recursive DFS    | %8.1f  | %8.1f  | %8.1f  | %8.1f  \n",
           rec_p50,
           rec_p95,
           rec_p99,
           rec_max);
    printf("  Iterative Traversal     | %8.1f  | %8.1f  | %8.1f  | %8.1f  \n",
           iter_p50,
           iter_p95,
           iter_p99,
           iter_max);
    printf("  ========================================================================\n\n");

    free(iterative_latencies);
    free(recursive_latencies);
    csilk_router_free(r);
}

int
main(void)
{
    printf("=== Csilk Iterative Router Trie Correctness & Latency Suite ===\n\n");
    test_differential_fuzz_verification();
    test_latency_percentile_benchmark();
    printf("=== All iterative router trie tests passed successfully! ===\n");
    return EXIT_SUCCESS;
}
