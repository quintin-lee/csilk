/**
 * @file test_http1_pipeline_bench.c
 * @brief Stage-level HTTP/1 parsing and request preparation benchmark.
 */

#include <llhttp.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_internal.h"
#include "core/primitives/header_map.h"
#include "core/primitives/router_internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

#define HTTP1_BENCH_ITERS 100000U
#define HTTP1_PARSE_ROUNDS 5

static volatile size_t sink;

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
on_url(llhttp_t* parser, const char* data, size_t length)
{
    (void)parser;
    sink += length;
    if (data && length > 0) {
        sink += (unsigned char)data[0];
    }
    return 0;
}

static int
on_header(llhttp_t* parser, const char* data, size_t length)
{
    (void)parser;
    sink += length;
    if (data && length > 0) {
        sink += (unsigned char)data[0];
    }
    return 0;
}

static int
on_body(llhttp_t* parser, const char* data, size_t length)
{
    (void)parser;
    (void)data;
    sink += length;
    return 0;
}

static int
on_complete(llhttp_t* parser)
{
    (void)parser;
    sink++;
    return 0;
}

static void
print_stage(const char* name, uint64_t elapsed, size_t operations)
{
    printf("HTTP1 stage=%s operations=%zu ns_per_op=%.2f\n",
           name,
           operations,
           (double)elapsed / (double)operations);
}

static int
compare_u64(const void* left, const void* right)
{
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}

static double
confidence95_half_width(const uint64_t* values, size_t count, double mean)
{
    if (count < 2) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        double delta = (double)values[i] - mean;
        sum += delta * delta;
    }
    return 2.7764451051977987 * sqrt(sum / (double)(count - 1)) / (double)count;
}

static void
run_parse_benchmark(const char* name, const char* request, size_t length)
{
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header;
    settings.on_header_value = on_header;
    settings.on_body = on_body;
    settings.on_message_complete = on_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);

    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        llhttp_errno_t error = llhttp_execute(&parser, request, length);
        if (error != HPE_OK) {
            fprintf(
                stderr, "HTTP/1 parser failed at iteration %zu: %s\n", i, llhttp_errno_name(error));
            abort();
        }
        llhttp_reset(&parser);
    }
    print_stage(name, now_ns() - start, HTTP1_BENCH_ITERS);
}

static void
run_fragmented_parse_benchmark(void)
{
    static const char* fragments[] = {
        "GET /fragmented HTTP/1.1\r\n",
        "Host: localhost\r\n",
        "User-Agent: csilk\r\n",
        "Accept: */*\r\n",
        "\r\n",
    };
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header;
    settings.on_header_value = on_header;
    settings.on_message_complete = on_complete;
    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        for (size_t fragment = 0; fragment < sizeof(fragments) / sizeof(fragments[0]); fragment++) {
            const char* chunk = fragments[fragment];
            size_t      length = strlen(chunk);
            if (llhttp_execute(&parser, chunk, length) != HPE_OK) {
                abort();
            }
        }
        llhttp_reset(&parser);
    }
    print_stage("parse_fragmented", now_ns() - start, HTTP1_BENCH_ITERS);
}

static void
run_header_matrix_benchmark(int header_count)
{
    char request[8192];
    int offset = snprintf(request, sizeof(request), "GET /headers HTTP/1.1\r\nHost: localhost\r\n");
    for (int i = 0; i < header_count && offset > 0 && (size_t)offset < sizeof(request); i++) {
        offset += snprintf(request + offset,
                           sizeof(request) - (size_t)offset,
                           "X-Benchmark-%d: value-%d\r\n",
                           i,
                           i);
    }
    offset += snprintf(request + offset, sizeof(request) - (size_t)offset, "\r\n");
    run_parse_benchmark("parse_headers", request, (size_t)offset);
}

static void
run_parse_rounds(const char* name, const char* request, size_t length)
{
    uint64_t samples[HTTP1_PARSE_ROUNDS];
    for (size_t round = 0; round < HTTP1_PARSE_ROUNDS; round++) {
        uint64_t start = now_ns();
        run_parse_benchmark(name, request, length);
        samples[round] = now_ns() - start;
    }
    qsort(samples, HTTP1_PARSE_ROUNDS, sizeof(samples[0]), compare_u64);
    double mean = 0.0;
    for (size_t i = 0; i < HTTP1_PARSE_ROUNDS; i++) {
        mean += (double)samples[i];
    }
    mean /= (double)HTTP1_PARSE_ROUNDS;
    printf("HTTP1 parse_summary=%s rounds=%d median_total_ns=%" PRIu64
           " mean_total_ns=%.0f ci95_ns=+/-%.0f\n",
           name,
           HTTP1_PARSE_ROUNDS,
           samples[HTTP1_PARSE_ROUNDS / 2],
           mean,
           confidence95_half_width(samples, HTTP1_PARSE_ROUNDS, mean));
}

static void
run_copy_benchmark(const char* name, const char* request, size_t length)
{
    char     buffer[8192];
    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        memcpy(buffer, request, length);
        sink += buffer[length - 1];
    }
    print_stage(name, now_ns() - start, HTTP1_BENCH_ITERS);
}

static void
dummy_handler(csilk_ctx_t* context)
{
    sink += context != NULL;
}

static void
run_context_header_benchmark(void)
{
    csilk_ctx_t* context = csilk_test_ctx_new();
    if (!context) {
        abort();
    }

    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        csilk_set_request_header(context, "Authorization", "Bearer token");
        sink += csilk_get_header(context, "Authorization") != NULL;
        csilk_ctx_cleanup(context);
    }
    print_stage("context_header_prepare", now_ns() - start, HTTP1_BENCH_ITERS);
    csilk_test_ctx_free(context);
}

static void
run_router_benchmark(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_handler_t handlers[] = {dummy_handler};
    if (!router || csilk_router_add(router, "GET", "/api/v1/users/:id", handlers, 1) != 0 ||
        csilk_router_compile(router, NULL, 0) != 0) {
        abort();
    }

    csilk_server_t server;
    memset(&server, 0, sizeof(server));
    atomic_init(&server.runtime_config.enable_simd, 1);
    csilk_ctx_t* context = csilk_test_ctx_new();
    if (!context) {
        abort();
    }
    context->server = &server;
    context->request.method = "GET";
    context->request.path = "/api/v1/users/42";

    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        sink += csilk_router_match_ctx(router, context) == 0;
    }
    print_stage("router_match", now_ns() - start, HTTP1_BENCH_ITERS);

    context->request.path = NULL;
    csilk_test_ctx_free(context);
    csilk_router_free(router);
}

static void
run_response_serialization_benchmark(void)
{
    csilk_ctx_t* context = csilk_test_ctx_new();
    if (!context) {
        abort();
    }
    const char* body = "{\"status\":\"ok\"}";
    context->response.status = 200;
    context->response.body = body;
    context->response.body_len = strlen(body);
    context->response.body_ownership = CSILK_OWN_BORROWED;
    csilk_set_header(context, "Content-Type", "application/json");
    size_t capacity = 512;
    char*  output = malloc(capacity);
    if (!output) {
        abort();
    }
    uint64_t start = now_ns();
    for (size_t i = 0; i < HTTP1_BENCH_ITERS; i++) {
        size_t length = _csilk_serialize_http1_response(context, output, capacity);
        if (length == 0) {
            abort();
        }
        sink += output[0];
    }
    print_stage("response_serialize", now_ns() - start, HTTP1_BENCH_ITERS);
    free(output);
    context->response.body = NULL;
    context->response.body_len = 0;
    context->response.body_ownership = CSILK_OWN_NONE;
    csilk_test_ctx_free(context);
}

int
main(void)
{
    static const char tiny_request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    static const char json_request[] = "POST /api/v1/users/42 HTTP/1.1\r\n"
                                       "Host: api.csilk.io\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: 26\r\n"
                                       "Authorization: Bearer token\r\n"
                                       "\r\n"
                                       "{\"name\":\"csilk\",\"ok\":true}";
    static const char body_request[] =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 64\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Request-ID: benchmark\r\n"
        "\r\n"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    run_parse_rounds("parse_tiny", tiny_request, sizeof(tiny_request) - 1);
    run_parse_rounds("parse_json", json_request, sizeof(json_request) - 1);
    run_parse_rounds("parse_body", body_request, sizeof(body_request) - 1);
    run_fragmented_parse_benchmark();
    run_header_matrix_benchmark(5);
    run_header_matrix_benchmark(10);
    run_header_matrix_benchmark(20);
    run_header_matrix_benchmark(50);
    run_context_header_benchmark();
    run_router_benchmark();
    run_response_serialization_benchmark();
    run_copy_benchmark("read_buffer_copy_json", json_request, sizeof(json_request) - 1);
    printf("HTTP1 sink=%zu\n", sink);
    return 0;
}
