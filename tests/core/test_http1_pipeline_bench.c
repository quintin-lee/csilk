/**
 * @file test_http1_pipeline_bench.c
 * @brief Stage-level HTTP/1 parsing and request preparation benchmark.
 */

#include <llhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
            return;
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
    run_copy_benchmark("read_buffer_copy_json", json_request, sizeof(json_request) - 1);
    printf("HTTP1 sink=%zu\n", sink);
    return 0;
}
