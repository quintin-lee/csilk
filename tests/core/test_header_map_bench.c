/**
 * @file test_header_map_bench.c
 * @brief Comprehensive correctness tests and CPU cycle / throughput benchmark for csilk_header_map.
 * @copyright MIT License
 */

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/test/test.h"
#include "core/ctx/ctx_internal.h"
#include "core/primitives/header_map.h"

/* ====================================================================
 * Cycle and Time Measurement
 * ==================================================================== */

#if defined(__x86_64__)
static inline uint64_t
get_cpu_cycles(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t
get_cpu_cycles(void)
{
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
static inline uint64_t
get_cpu_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

static inline uint64_t
get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ====================================================================
 * 1. Correctness & Invariants Test Suite
 * ==================================================================== */

static void
test_header_map_correctness(void)
{
    printf("Testing Header Map Invariants and Edge Cases...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    csilk_header_map_t map;
    memset(&map, 0, sizeof(map));

    /* 1. Basic map_set & case-insensitive map_get */
    map_set(c, &map, "Content-Type", "application/json");
    map_set(c, &map, "Host", "example.com:8080");
    map_set(c, &map, "Authorization", "Bearer secret-token-12345");

    assert(map_get(&map, "Content-Type") != NULL &&
           strcmp(map_get(&map, "Content-Type"), "application/json") == 0);
    assert(map_get(&map, "content-type") != NULL &&
           strcmp(map_get(&map, "content-type"), "application/json") == 0);
    assert(map_get(&map, "CONTENT-TYPE") != NULL &&
           strcmp(map_get(&map, "CONTENT-TYPE"), "application/json") == 0);
    assert(map_get(&map, "cOnTeNt-TyPe") != NULL &&
           strcmp(map_get(&map, "cOnTeNt-TyPe"), "application/json") == 0);
    assert(map_get(&map, "host") != NULL && strcmp(map_get(&map, "host"), "example.com:8080") == 0);
    assert(map_get(&map, "authorization") != NULL &&
           strcmp(map_get(&map, "authorization"), "Bearer secret-token-12345") == 0);
    assert(map_get(&map, "Non-Existent-Header") == NULL);

    /* 2. map_get_view */
    csilk_view_t v = map_get_view(&map, "Content-Type");
    assert(v.len == strlen("application/json") && memcmp(v.ptr, "application/json", v.len) == 0);

    /* 3. map_set overwrite behavior */
    map_set(c, &map, "Content-Type", "text/html; charset=utf-8");
    assert(strcmp(map_get(&map, "content-type"), "text/html; charset=utf-8") == 0);

    /* 4. map_set_view zero-copy input */
    csilk_str_view_t kv = {"X-Custom-Slice-Header", 21};
    csilk_str_view_t vv = {"SliceValue123", 13};
    map_set_view(c, &map, &kv, &vv);
    assert(map_get(&map, "x-custom-slice-header") != NULL);
    assert(strcmp(map_get(&map, "x-custom-slice-header"), "SliceValue123") == 0);

    /* 5. Duplicate header support via map_add */
    map_add(c, &map, "Set-Cookie", "session=123; Path=/");
    map_add(c, &map, "Set-Cookie", "theme=dark; Path=/");
    assert(map_get(&map, "Set-Cookie") != NULL);

    /* Count nodes with key Set-Cookie in bucket */
    uint32_t b = hash_key("Set-Cookie");
    int      cookie_count = 0;
    for (csilk_header_t* h = map.buckets[b]; h; h = h->next) {
        if (strcasecmp(h->key, "Set-Cookie") == 0) {
            cookie_count++;
        }
    }
    assert(cookie_count == 2);

    /* 6. Very long header (>128 bytes) */
    char long_key[256];
    memset(long_key, 'x', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    map_set(c, &map, long_key, "long_value");
    assert(map_get(&map, long_key) != NULL && strcmp(map_get(&map, long_key), "long_value") == 0);

    csilk_test_ctx_free(c);
    printf("  Header Map Invariants: 100%% Clean!\n\n");
}

/* ====================================================================
 * 2. Legacy Reference for Benchmark Comparison
 * ==================================================================== */

static inline uint32_t
legacy_hash_key(const char* key)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*key++)) {
        hash = ((hash << 5) + hash) + tolower(c);
    }
    return hash % CSILK_HEADER_BUCKETS;
}

static const char*
legacy_map_get(csilk_header_map_t* map, const char* key)
{
    if (!map || !key) {
        return NULL;
    }
    uint32_t        bucket = legacy_hash_key(key);
    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (strcasecmp(h->key, key) == 0) {
            return h->value;
        }
        h = h->next;
    }
    return NULL;
}

/* ====================================================================
 * 3. Benchmark: 8, 16, 32, 64 Headers Per Request
 * ==================================================================== */

static const char* const TEST_HEADERS_POOL[] = {"Host",
                                                "User-Agent",
                                                "Accept",
                                                "Accept-Language",
                                                "Accept-Encoding",
                                                "Connection",
                                                "Upgrade-Insecure-Requests",
                                                "Sec-Fetch-Dest",
                                                "Sec-Fetch-Mode",
                                                "Sec-Fetch-Site",
                                                "Sec-Fetch-User",
                                                "Cache-Control",
                                                "Authorization",
                                                "Content-Type",
                                                "Content-Length",
                                                "Cookie",
                                                "Origin",
                                                "Referer",
                                                "If-Modified-Since",
                                                "If-None-Match",
                                                "X-Forwarded-For",
                                                "X-Forwarded-Proto",
                                                "X-Real-IP",
                                                "X-Request-ID",
                                                "X-Correlation-ID",
                                                "X-Frame-Options",
                                                "X-XSS-Protection",
                                                "X-Content-Type-Options",
                                                "Strict-Transport-Security",
                                                "Access-Control-Allow-Origin",
                                                "Access-Control-Allow-Methods",
                                                "Access-Control-Allow-Headers",
                                                "Access-Control-Max-Age",
                                                "ETag",
                                                "Server",
                                                "Date",
                                                "Expires",
                                                "Last-Modified",
                                                "Location",
                                                "Set-Cookie",
                                                "Transfer-Encoding",
                                                "Vary",
                                                "Keep-Alive",
                                                "Proxy-Connection",
                                                "TE",
                                                "Trailer",
                                                "Upgrade",
                                                "Via",
                                                "Warning",
                                                "Allow",
                                                "Content-Encoding",
                                                "Content-Language",
                                                "Content-Location",
                                                "Content-MD5",
                                                "Content-Range",
                                                "Accept-Ranges",
                                                "Age",
                                                "Proxy-Authenticate",
                                                "WWW-Authenticate",
                                                "Pragma",
                                                "X-RateLimit-Limit",
                                                "X-RateLimit-Remaining",
                                                "X-RateLimit-Reset",
                                                "X-Trace-ID"};

static void
test_header_map_benchmarks(void)
{
    printf("Benchmarking Header Map Lookup Performance Across Header Counts...\n");

    const size_t header_counts[] = {8, 16, 32, 64};
    const size_t num_counts = sizeof(header_counts) / sizeof(header_counts[0]);
    const int    NUM_LOOKUPS = 1000000;

    printf("  "
           "======================================================================================="
           "============\n");
    printf("  Headers/Req | Engine           | Cycles/Lookup | Latency (ns/op) | Speedup vs Legacy "
           "(Cycles)     \n");
    printf("  "
           "------------+------------------+---------------+-----------------+---------------------"
           "-----------\n");

    for (size_t ci = 0; ci < num_counts; ci++) {
        size_t count = header_counts[ci];

        csilk_ctx_t*       c = csilk_test_ctx_new();
        csilk_header_map_t map;
        memset(&map, 0, sizeof(map));

        /* Populate map with `count` realistic headers */
        for (size_t i = 0; i < count; i++) {
            char val[64];
            snprintf(val, sizeof(val), "header-value-%zu", i);
            map_set(c, &map, TEST_HEADERS_POOL[i], val);
        }

        /* 1. Benchmark Legacy map_get */
        uint64_t     t0 = get_time_ns();
        uint64_t     c0 = get_cpu_cycles();
        volatile int found_legacy = 0;
        for (int i = 0; i < NUM_LOOKUPS; i++) {
            const char* key = TEST_HEADERS_POOL[i % count];
            const char* v = legacy_map_get(&map, key);
            if (v) {
                found_legacy++;
            }
        }
        uint64_t c1 = get_cpu_cycles();
        uint64_t t1 = get_time_ns();

        double legacy_cycles = (double)(c1 - c0) / NUM_LOOKUPS;
        double legacy_ns = (double)(t1 - t0) / NUM_LOOKUPS;

        /* 2. Benchmark Optimized map_get (Hash + Key_len + Memcmp) */
        uint64_t     t2 = get_time_ns();
        uint64_t     c2 = get_cpu_cycles();
        volatile int found_opt = 0;
        for (int i = 0; i < NUM_LOOKUPS; i++) {
            const char* key = TEST_HEADERS_POOL[i % count];
            const char* v = map_get(&map, key);
            if (v) {
                found_opt++;
            }
        }
        uint64_t c3 = get_cpu_cycles();
        uint64_t t3 = get_time_ns();

        double opt_cycles = (double)(c3 - c2) / NUM_LOOKUPS;
        double opt_ns = (double)(t3 - t2) / NUM_LOOKUPS;

        double speedup_cycles = legacy_cycles / (opt_cycles > 0 ? opt_cycles : 1.0);

        printf("  %2zu headers  | Legacy (strcase) | %11.1f   | %13.2f   | baseline                "
               "       \n",
               count,
               legacy_cycles,
               legacy_ns);
        printf("              | Optimized (Fast) | %11.1f   | %13.2f   | %5.2fx faster             "
               "     \n",
               opt_cycles,
               opt_ns,
               speedup_cycles);
        printf("  "
               "------------+------------------+---------------+-----------------+-----------------"
               "---------------\n");

        csilk_test_ctx_free(c);
    }
    printf("  "
           "======================================================================================="
           "============\n\n");
}

int
main(void)
{
    printf("=== Csilk HTTP Header Map Performance & Verification Suite ===\n\n");
    test_header_map_correctness();
    test_header_map_benchmarks();
    printf("=== All Header Map Tests and Benchmarks Passed Successfully! ===\n");
    return EXIT_SUCCESS;
}
