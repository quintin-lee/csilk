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

int
main()
{
    test_otlp_exporter_basic();
    printf("All OTLP Exporter tests passed successfully!\n");
    return 0;
}
