#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

static void
test_otlp_exporter_basic()
{
    printf("Testing OpenTelemetry OTLP Exporter...\n");

    csilk_otlp_exporter_t* exp = csilk_otlp_exporter_new("http://localhost:4318/v1/traces", 10);
    assert(exp != NULL);

    csilk_otlp_span_t span1 = {.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
                               .span_id = "00f067aa0ba902b7",
                               .parent_span_id = "",
                               .name = "GET /api/v1/users",
                               .start_time_ns = 1700000000000000000ULL,
                               .end_time_ns = 1700000000050000000ULL,
                               .status_code = 200};

    csilk_otlp_exporter_record_span(exp, &span1);

    char json_buf[1024] = {0};
    int  count = csilk_otlp_exporter_export_json(exp, json_buf, sizeof(json_buf));

    assert(count == 1);
    assert(strstr(json_buf, "resourceSpans") != NULL);
    assert(strstr(json_buf, "4bf92f3577b34da6a3ce929d0e0e4736") != NULL);
    assert(strstr(json_buf, "GET /api/v1/users") != NULL);

    printf("[OTLP Exporter JSON] %s\n", json_buf);
    csilk_otlp_exporter_free(exp);

    printf("test_otlp_exporter_basic: PASS\n");
}

static void
test_otlp_exporter_export_grpc()
{
    printf("Testing OpenTelemetry OTLP/gRPC Exporter...\n");

    csilk_otlp_exporter_t* exp = csilk_otlp_exporter_new("http://localhost:4317", 10);
    assert(exp != NULL);

    csilk_otlp_span_t span1 = {.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736",
                               .span_id = "00f067aa0ba902b7",
                               .parent_span_id = "",
                               .name = "POST /api/v1/grpc",
                               .start_time_ns = 1700000000000000000ULL,
                               .end_time_ns = 1700000000050000000ULL,
                               .status_code = 200};

    csilk_otlp_exporter_record_span(exp, &span1);

    uint8_t grpc_buf[2048] = {0};
    int     frame_len = csilk_otlp_exporter_export_grpc(exp, grpc_buf, sizeof(grpc_buf));

    assert(frame_len > 5);
    /* Verify 5-byte gRPC prefix header: Compressed-Flag = 0 */
    assert(grpc_buf[0] == 0x00);

    csilk_otlp_exporter_free(exp);
    printf("test_otlp_exporter_export_grpc: PASS\n");
}

static void
_test_flush_collector(const char* json, void* user_data)
{
    int* flush_count = (int*)user_data;
    if (json && strstr(json, "resourceSpans")) {
        (*flush_count)++;
    }
}

static void
test_otlp_exporter_background_worker(void)
{
    printf("Testing OpenTelemetry OTLP Exporter Background Worker...\n");

    csilk_otlp_exporter_t* exp = csilk_otlp_exporter_new("http://localhost:4318/v1/traces", 5);
    assert(exp != NULL);

    int flush_count = 0;
    csilk_otlp_exporter_set_flush_cb(exp, _test_flush_collector, &flush_count);

    int rc = csilk_otlp_exporter_start(exp, 50); /* 50ms flush interval */
    assert(rc == 0);

    csilk_otlp_span_t span = {.trace_id = "11112222333344445555666677778888",
                              .span_id = "1234567890abcdef",
                              .name = "WorkerTask",
                              .start_time_ns = 1000,
                              .end_time_ns = 2000,
                              .status_code = 1};
    csilk_otlp_exporter_record_span(exp, &span);

    /* Sleep 120ms to allow background worker to flush */
    csilk_io_sleep(120);

    assert(flush_count >= 1);

    csilk_otlp_exporter_stop(exp);
    csilk_otlp_exporter_free(exp);
    printf("test_otlp_exporter_background_worker: PASS\n");
}

int
main()
{
    test_otlp_exporter_basic();
    test_otlp_exporter_export_grpc();
    test_otlp_exporter_background_worker();
    printf("All OTLP Exporter tests passed successfully!\n");
    return 0;
}
