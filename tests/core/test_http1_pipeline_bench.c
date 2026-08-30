/**
 * @file test_http1_pipeline_bench.c
 * @brief Stage-level HTTP/1 parsing and request preparation benchmark.
 */

#include <llhttp.h>
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
run_copy_benchmark(const char* name, const char* request, size_t length)
{
    char     buffer[1024];
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

    context->server = NULL;
    context->arena = NULL;
    free(context);
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

    run_parse_benchmark("parse_tiny", tiny_request, sizeof(tiny_request) - 1);
    run_parse_benchmark("parse_json", json_request, sizeof(json_request) - 1);
    run_context_header_benchmark();
    run_router_benchmark();
    run_response_serialization_benchmark();
    run_copy_benchmark("read_buffer_copy_json", json_request, sizeof(json_request) - 1);
    printf("HTTP1 sink=%zu\n", sink);
    return 0;
}
