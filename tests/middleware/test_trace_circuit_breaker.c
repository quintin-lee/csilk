#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_otlp_trace_middleware()
{
    printf("Testing OpenTelemetry W3C Trace Context middleware...\n");

    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_trace_middleware(ctx);

    const char* trace_id = csilk_ctx_get_trace_id(ctx);
    const char* span_id = csilk_ctx_get_span_id(ctx);

    assert(trace_id != NULL);
    assert(span_id != NULL);
    assert(strlen(trace_id) == 32);
    assert(strlen(span_id) == 16);

    const char* tp_hdr = csilk_get_response_header(ctx, "traceparent");
    assert(tp_hdr != NULL);

    printf("[OTLP Trace] Generated Trace ID: %s, Span ID: %s, Header: %s\n",
           trace_id,
           span_id,
           tp_hdr);
    csilk_test_ctx_free(ctx);
    printf("test_otlp_trace_middleware: PASS\n");
}

static void
test_circuit_breaker_middleware()
{
    printf("Testing Circuit Breaker middleware...\n");

    csilk_circuit_breaker_config_t cfg = {.failure_threshold = 2, .recovery_timeout_ms = 1000};
    csilk_circuit_breaker_t*       cb = csilk_circuit_breaker_new(&cfg);
    assert(cb != NULL);

    assert(csilk_circuit_breaker_get_state(cb) == 0); // CLOSED

    csilk_circuit_breaker_record_failure(cb);
    assert(csilk_circuit_breaker_get_state(cb) == 0); // STILL CLOSED

    csilk_circuit_breaker_record_failure(cb);
    assert(csilk_circuit_breaker_get_state(cb) == 1); // OPEN

    csilk_ctx_t*    ctx = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_circuit_breaker_middleware(ctx, cb);
    assert(csilk_get_status(ctx) == 503);
    csilk_test_ctx_free(ctx);

    csilk_circuit_breaker_record_success(cb);
    csilk_circuit_breaker_free(cb);
    printf("test_circuit_breaker_middleware: PASS\n");
}

int
main()
{
    test_otlp_trace_middleware();
    test_circuit_breaker_middleware();
    printf("All OTLP Trace & Circuit Breaker tests passed successfully!\n");
    return 0;
}
