/**
 * @file test_h2_header_bench.c
 * @brief Benchmark and correctness verification for HTTP/2 Header Materialization and Single-Pass Response Encoding.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/http/h2.h"
#include "csilk/test/test.h"
#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/header_map.h"
#include <nghttp2/nghttp2.h>

#if defined(__x86_64__)
static inline uint64_t
get_cpu_cycles(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#else
static inline uint64_t
get_cpu_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static inline uint64_t
get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int on_header_callback(nghttp2_session*     session,
                       const nghttp2_frame* frame,
                       const uint8_t*       name,
                       size_t               namelen,
                       const uint8_t*       value,
                       size_t               valuelen,
                       uint8_t              flags,
                       void*                user_data);

/* -------------------------------------------------------------------------- */
/* Benchmark: Materializing N Headers into Request Context                    */
/* -------------------------------------------------------------------------- */
static void
benchmark_h2_header_materialization(int header_count, int iterations)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.protocol = CSILK_PROTO_HTTP2;
    c->_internal_client = &client;
    c->h2_stream_owner = &client;

    nghttp2_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.hd.type = NGHTTP2_HEADERS;
    frame.hd.stream_id = 1;
    frame.headers.cat = NGHTTP2_HCAT_REQUEST;

    char keys[100][32];
    char vals[100][64];
    for (int i = 0; i < 100; i++) {
        snprintf(keys[i], sizeof(keys[i]), "x-custom-header-%03d", i);
        snprintf(vals[i], sizeof(vals[i]), "custom-header-value-%03d-data", i);
    }

    uint64_t* latencies = malloc(sizeof(uint64_t) * (size_t)iterations);
    assert(latencies != NULL);

    uint64_t start_cycles = get_cpu_cycles();
    uint64_t start_ns = get_time_ns();

    for (int it = 0; it < iterations; it++) {
        csilk_arena_reset(c->arena);
        c->request.headers.used = 0;
        c->request.headers.count = 0;
        memset(&c->request.headers, 0, sizeof(c->request.headers));

        uint64_t it_start = get_time_ns();

        /* Feed headers */
        for (int h = 0; h < header_count; h++) {
            on_header_callback(NULL,
                               &frame,
                               (const uint8_t*)keys[h],
                               strlen(keys[h]),
                               (const uint8_t*)vals[h],
                               strlen(vals[h]),
                               0,
                               &client);
        }

        latencies[it] = get_time_ns() - it_start;
    }

    uint64_t elapsed_ns = get_time_ns() - start_ns;
    uint64_t elapsed_cycles = get_cpu_cycles() - start_cycles;

    /* Sort latencies for percentiles */
    for (int i = 0; i < iterations - 1; i++) {
        for (int j = i + 1; j < iterations; j++) {
            if (latencies[i] > latencies[j]) {
                uint64_t tmp = latencies[i];
                latencies[i] = latencies[j];
                latencies[j] = tmp;
            }
        }
    }

    uint64_t p50 = latencies[iterations * 50 / 100];
    uint64_t p95 = latencies[iterations * 95 / 100];
    uint64_t p99 = latencies[iterations * 99 / 100];

    double ns_per_op = (double)elapsed_ns / (double)iterations;
    double cycles_per_op = (double)elapsed_cycles / (double)iterations;
    double mops = ((double)iterations / ((double)elapsed_ns / 1e9)) / 1e6;

    printf("  [Headers = %3d] %8.1f ns/req | %8.1f cycles/req | p50: %4lu ns | p95: %4lu ns | p99: "
           "%4lu ns | %6.2f M req/s\n",
           header_count,
           ns_per_op,
           cycles_per_op,
           p50,
           p95,
           p99,
           mops);

    free(latencies);
    c->_internal_client = NULL;
    csilk_test_ctx_free(c);
}

/* -------------------------------------------------------------------------- */
/* Test: Response Encoding Count Correctness                                  */
/* -------------------------------------------------------------------------- */
static void
test_h2_response_header_count_correctness(void)
{
    printf("Testing HTTP/2 Response Header Count Correctness...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    assert(c->response.headers.count == 0);
    csilk_set_header(c, "Content-Type", "application/json");
    assert(c->response.headers.count == 1);

    csilk_set_header(c, "Server", "CSilk/0.5.2");
    assert(c->response.headers.count == 2);

    csilk_set_header(c, "X-Request-ID", "req-12345");
    assert(c->response.headers.count == 3);

    /* Overwrite existing header should NOT increment count */
    csilk_set_header(c, "Content-Type", "text/plain");
    assert(c->response.headers.count == 3);

    csilk_test_ctx_free(c);
    printf("test_h2_response_header_count_correctness: PASS\n");
}

int
main(void)
{
    printf("=== Running HTTP/2 Header Materialization & Encoding Benchmarks ===\n\n");
    test_h2_response_header_count_correctness();

    printf("\n--- Header Materialization Benchmark (5,000 iterations per tier) ---\n");
    const int ITERS = 5000;
    benchmark_h2_header_materialization(0, ITERS);
    benchmark_h2_header_materialization(5, ITERS);
    benchmark_h2_header_materialization(20, ITERS);
    benchmark_h2_header_materialization(50, ITERS);
    benchmark_h2_header_materialization(100, ITERS);

    printf("\n=== All HTTP/2 Header Benchmarks Completed Successfully! ===\n");
    return 0;
}
