/**
 * @file test_header_map_bench.c
 * @brief Comprehensive correctness tests and CPU cycle / throughput benchmark for Header Name Interning & ID Fast Path.
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
test_header_interning_correctness(void)
{
    printf("Testing Header Name Interning Correctness & Case-Insensitivity...\n");

    /* Verify all known standard headers */
    assert(csilk_header_id_from_name("Host", 4) == CSILK_HDR_HOST);
    assert(csilk_header_id_from_name("host", 4) == CSILK_HDR_HOST);
    assert(csilk_header_id_from_name("HOST", 4) == CSILK_HDR_HOST);
    assert(csilk_header_id_from_name("Content-Type", 12) == CSILK_HDR_CONTENT_TYPE);
    assert(csilk_header_id_from_name("content-type", 12) == CSILK_HDR_CONTENT_TYPE);
    assert(csilk_header_id_from_name("CONTENT-TYPE", 12) == CSILK_HDR_CONTENT_TYPE);
    assert(csilk_header_id_from_name("Content-Length", 14) == CSILK_HDR_CONTENT_LENGTH);
    assert(csilk_header_id_from_name("Authorization", 13) == CSILK_HDR_AUTHORIZATION);
    assert(csilk_header_id_from_name("Cookie", 6) == CSILK_HDR_COOKIE);
    assert(csilk_header_id_from_name("Set-Cookie", 10) == CSILK_HDR_SET_COOKIE);
    assert(csilk_header_id_from_name("Accept", 6) == CSILK_HDR_ACCEPT);
    assert(csilk_header_id_from_name("Accept-Encoding", 15) == CSILK_HDR_ACCEPT_ENCODING);
    assert(csilk_header_id_from_name("Accept-Language", 15) == CSILK_HDR_ACCEPT_LANGUAGE);
    assert(csilk_header_id_from_name("User-Agent", 10) == CSILK_HDR_USER_AGENT);
    assert(csilk_header_id_from_name("Connection", 10) == CSILK_HDR_CONNECTION);
    assert(csilk_header_id_from_name("Upgrade", 7) == CSILK_HDR_UPGRADE);
    assert(csilk_header_id_from_name("Cache-Control", 13) == CSILK_HDR_CACHE_CONTROL);
    assert(csilk_header_id_from_name("Origin", 6) == CSILK_HDR_ORIGIN);
    assert(csilk_header_id_from_name("Referer", 7) == CSILK_HDR_REFERER);
    assert(csilk_header_id_from_name("Sec-WebSocket-Key", 17) == CSILK_HDR_SEC_WEBSOCKET_KEY);
    assert(csilk_header_id_from_name("Sec-WebSocket-Version", 21) ==
           CSILK_HDR_SEC_WEBSOCKET_VERSION);
    assert(csilk_header_id_from_name("Sec-WebSocket-Extensions", 24) ==
           CSILK_HDR_SEC_WEBSOCKET_EXTENSIONS);
    assert(csilk_header_id_from_name("Sec-WebSocket-Protocol", 22) ==
           CSILK_HDR_SEC_WEBSOCKET_PROTOCOL);
    assert(csilk_header_id_from_name("Transfer-Encoding", 17) == CSILK_HDR_TRANSFER_ENCODING);
    assert(csilk_header_id_from_name("Location", 8) == CSILK_HDR_LOCATION);
    assert(csilk_header_id_from_name("If-Modified-Since", 17) == CSILK_HDR_IF_MODIFIED_SINCE);
    assert(csilk_header_id_from_name("If-None-Match", 13) == CSILK_HDR_IF_NONE_MATCH);
    assert(csilk_header_id_from_name("ETag", 4) == CSILK_HDR_ETAG);
    assert(csilk_header_id_from_name("Server", 6) == CSILK_HDR_SERVER);
    assert(csilk_header_id_from_name("Date", 4) == CSILK_HDR_DATE);
    assert(csilk_header_id_from_name("Vary", 4) == CSILK_HDR_VARY);
    assert(csilk_header_id_from_name("X-Request-ID", 12) == CSILK_HDR_X_REQUEST_ID);
    assert(csilk_header_id_from_name("X-Forwarded-For", 15) == CSILK_HDR_X_FORWARDED_FOR);
    assert(csilk_header_id_from_name("X-Real-IP", 9) == CSILK_HDR_X_REAL_IP);
    assert(csilk_header_id_from_name("Content-Encoding", 16) == CSILK_HDR_CONTENT_ENCODING);

    /* Unknown / custom headers */
    assert(csilk_header_id_from_name("X-Custom-Auth-Token", 19) == CSILK_HDR_UNKNOWN);
    assert(csilk_header_id_from_name("X-RateLimit-Remaining", 21) == CSILK_HDR_UNKNOWN);
    assert(csilk_header_id_from_name("SomethingElse", 13) == CSILK_HDR_UNKNOWN);
    assert(csilk_header_id_from_name(NULL, 0) == CSILK_HDR_UNKNOWN);

    /* Canonical names */
    assert(strcmp(csilk_header_id_name(CSILK_HDR_HOST), "Host") == 0);
    assert(strcmp(csilk_header_id_name(CSILK_HDR_CONTENT_TYPE), "Content-Type") == 0);
    assert(strcmp(csilk_header_id_name(CSILK_HDR_UNKNOWN), "Unknown") == 0);

    printf("  Header Interning Invariants: 100%% Clean!\n");
}

static void
test_header_map_correctness(void)
{
    printf("Testing Header Map Invariants and Direct ID Accessors...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    /* 1. Set request headers */
    csilk_set_request_header(c, "Host", "example.com:8080");
    csilk_set_request_header(c, "Content-Type", "application/json");
    csilk_set_request_header(c, "Authorization", "Bearer secret-token-12345");
    csilk_set_request_header(c, "X-Custom-Service", "PaymentGateway/v2");

    /* 2. Direct ID lookups */
    const char* host_val = csilk_get_header_id(c, CSILK_HDR_HOST);
    assert(host_val != NULL && strcmp(host_val, "example.com:8080") == 0);

    const char* ct_val = csilk_get_header_id(c, CSILK_HDR_CONTENT_TYPE);
    assert(ct_val != NULL && strcmp(ct_val, "application/json") == 0);

    const char* auth_val = csilk_get_header_id(c, CSILK_HDR_AUTHORIZATION);
    assert(auth_val != NULL && strcmp(auth_val, "Bearer secret-token-12345") == 0);

    assert(csilk_get_header_id(c, CSILK_HDR_COOKIE) == NULL);

    /* 3. Direct ID zero-copy views */
    csilk_view_t hv = csilk_get_header_id_view(c, CSILK_HDR_HOST);
    assert(hv.len == strlen("example.com:8080") && memcmp(hv.ptr, "example.com:8080", hv.len) == 0);

    /* 4. String-based name lookups */
    assert(strcmp(csilk_get_header(c, "host"), "example.com:8080") == 0);
    assert(strcmp(csilk_get_header(c, "CONTENT-TYPE"), "application/json") == 0);
    assert(strcmp(csilk_get_header(c, "x-custom-service"), "PaymentGateway/v2") == 0);
    assert(csilk_get_header(c, "Non-Existent-Header") == NULL);

    /* 5. Response headers direct ID test */
    csilk_set_header(c, "Server", "Csilk/1.0");
    csilk_set_header(c, "Content-Type", "text/plain");
    assert(strcmp(csilk_get_response_header_id(c, CSILK_HDR_SERVER), "Csilk/1.0") == 0);
    assert(strcmp(csilk_get_response_header_id(c, CSILK_HDR_CONTENT_TYPE), "text/plain") == 0);

    /* 6. Overwrite check on known ID */
    csilk_set_header(c, "Server", "Csilk/2.0");
    assert(strcmp(csilk_get_response_header_id(c, CSILK_HDR_SERVER), "Csilk/2.0") == 0);
    assert(strcmp(csilk_get_response_header(c, "server"), "Csilk/2.0") == 0);

    csilk_test_ctx_free(c);
    printf("  Header Map & ID Accessors: 100%% Clean!\n\n");
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
 * 3. Benchmark: Realistic HTTP Request Workload
 * ==================================================================== */

static void
test_realistic_http_workload_bench(void)
{
    printf("Benchmarking Realistic HTTP Workload: String Lookup vs Interned ID Fast Path...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    /* Populate typical HTTP request headers */
    csilk_set_request_header(c, "Host", "api.csilk.dev:8443");
    csilk_set_request_header(
        c, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    csilk_set_request_header(
        c, "Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    csilk_set_request_header(c, "Accept-Language", "en-US,en;q=0.5");
    csilk_set_request_header(c, "Accept-Encoding", "gzip, deflate, br, zstd");
    csilk_set_request_header(c, "Connection", "keep-alive");
    csilk_set_request_header(
        c,
        "Authorization",
        "Bearer "
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ");
    csilk_set_request_header(
        c, "Cookie", "session_id=s%3A9876543210.signature; theme=dark; lang=en");
    csilk_set_request_header(c, "Content-Type", "application/json; charset=utf-8");
    csilk_set_request_header(c, "Content-Length", "2048");
    csilk_set_request_header(c, "X-Request-ID", "req_99887766554433221100");
    csilk_set_request_header(c, "X-Custom-Tenant-ID", "tenant-alpha-9988");

    const int NUM_REQUESTS = 1000000;

    /* A typical HTTP middleware + route handler pipeline inspects:
     *   1. Host
     *   2. Authorization
     *   3. Content-Type
     *   4. Content-Length
     *   5. Accept-Encoding
     *   6. Cookie
     *   7. User-Agent
     *   8. X-Request-ID
     *   (Total 8 header lookups per request)
     */

    /* --- Path 1: Legacy strcasecmp loop --- */
    uint64_t     t0 = get_time_ns();
    uint64_t     c0 = get_cpu_cycles();
    volatile int legacy_hits = 0;
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (legacy_map_get(&c->request.headers, "Host")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "Authorization")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "Content-Type")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "Content-Length")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "Accept-Encoding")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "Cookie")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "User-Agent")) {
            legacy_hits++;
        }
        if (legacy_map_get(&c->request.headers, "X-Request-ID")) {
            legacy_hits++;
        }
    }
    uint64_t c1 = get_cpu_cycles();
    uint64_t t1 = get_time_ns();
    double   legacy_total_cycles = (double)(c1 - c0) / NUM_REQUESTS;
    double   legacy_per_hdr_cycles = legacy_total_cycles / 8.0;
    double   legacy_ns = (double)(t1 - t0) / NUM_REQUESTS;

    /* --- Path 2: String lookup with Hash + Len + Memcmp --- */
    uint64_t     t2 = get_time_ns();
    uint64_t     c2 = get_cpu_cycles();
    volatile int str_hits = 0;
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (csilk_get_header(c, "Host")) {
            str_hits++;
        }
        if (csilk_get_header(c, "Authorization")) {
            str_hits++;
        }
        if (csilk_get_header(c, "Content-Type")) {
            str_hits++;
        }
        if (csilk_get_header(c, "Content-Length")) {
            str_hits++;
        }
        if (csilk_get_header(c, "Accept-Encoding")) {
            str_hits++;
        }
        if (csilk_get_header(c, "Cookie")) {
            str_hits++;
        }
        if (csilk_get_header(c, "User-Agent")) {
            str_hits++;
        }
        if (csilk_get_header(c, "X-Request-ID")) {
            str_hits++;
        }
    }
    uint64_t c3 = get_cpu_cycles();
    uint64_t t3 = get_time_ns();
    double   str_total_cycles = (double)(c3 - c2) / NUM_REQUESTS;
    double   str_per_hdr_cycles = str_total_cycles / 8.0;
    double   str_ns = (double)(t3 - t2) / NUM_REQUESTS;

    /* --- Path 3: Direct Interned Header ID (O(1) Array Indexing) --- */
    uint64_t     t4 = get_time_ns();
    uint64_t     c4 = get_cpu_cycles();
    volatile int id_hits = 0;
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (csilk_get_header_id(c, CSILK_HDR_HOST)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_AUTHORIZATION)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_CONTENT_TYPE)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_CONTENT_LENGTH)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_ACCEPT_ENCODING)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_COOKIE)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_USER_AGENT)) {
            id_hits++;
        }
        if (csilk_get_header_id(c, CSILK_HDR_X_REQUEST_ID)) {
            id_hits++;
        }
    }
    uint64_t c5 = get_cpu_cycles();
    uint64_t t5 = get_time_ns();
    double   id_total_cycles = (double)(c5 - c4) / NUM_REQUESTS;
    double   id_per_hdr_cycles = id_total_cycles / 8.0;
    double   id_ns = (double)(t5 - t4) / NUM_REQUESTS;

    printf("  "
           "======================================================================================="
           "=============\n");
    printf("  Lookup Mode                   | Total Cycles/Req | Cycles/Hdr | Time/Req (ns) | "
           "Speedup vs Legacy  \n");
    printf("  "
           "------------------------------+------------------+------------+---------------+--------"
           "------------\n");
    printf("  1. Legacy (strcasecmp loop)   | %16.1f | %10.1f | %13.2f | baseline           \n",
           legacy_total_cycles,
           legacy_per_hdr_cycles,
           legacy_ns);
    printf("  2. String Name (Hash + Len)   | %16.1f | %10.1f | %13.2f | %5.2fx faster      \n",
           str_total_cycles,
           str_per_hdr_cycles,
           str_ns,
           legacy_total_cycles / (str_total_cycles > 0 ? str_total_cycles : 1.0));
    printf("  3. Interned ID (Direct O(1))  | %16.1f | %10.1f | %13.2f | %5.2fx faster 🚀   \n",
           id_total_cycles,
           id_per_hdr_cycles,
           id_ns,
           legacy_total_cycles / (id_total_cycles > 0 ? id_total_cycles : 1.0));
    printf("  "
           "======================================================================================="
           "=============\n\n");

    csilk_test_ctx_free(c);
}

int
main(void)
{
    printf("=== Csilk HTTP Header Name Interning Performance & Verification Suite ===\n\n");
    test_header_interning_correctness();
    test_header_map_correctness();
    test_realistic_http_workload_bench();
    printf("=== All Header Interning Tests and Benchmarks Passed Successfully! ===\n");
    return EXIT_SUCCESS;
}
