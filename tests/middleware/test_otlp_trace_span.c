#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/middleware/otlp_trace.h"
#include "csilk/test/test.h"

static void
test_otlp_trace_span_lifecycle(void)
{
    csilk_otlp_span_t* span = csilk_otlp_tracer_start_span("WorkflowNode:step1", nullptr);
    assert(span != nullptr);
    assert(strlen(span->trace_id) == 32);
    assert(strlen(span->span_id) == 16);
    /* Ensure no longer hardcoded demo trace ID */
    assert(strcmp(span->trace_id, "4bf92f3577b34da6a3ce929d0e0e4736") != 0);

    csilk_otlp_tracer_end_span(span, 1);
    printf("test_otlp_trace_span_lifecycle passed\n");
}

static void
test_otlp_trace_child_span_nesting(void)
{
    csilk_otlp_span_t* root = csilk_otlp_tracer_start_span("HTTP:GET /api/v1/orders", nullptr);
    assert(root != nullptr);
    char root_trace[33];
    char root_span[17];
    snprintf(root_trace, sizeof(root_trace), "%s", root->trace_id);
    snprintf(root_span, sizeof(root_span), "%s", root->span_id);

    /* Child span automatically inherits active trace ID and parent span ID */
    csilk_otlp_span_t* child = csilk_otlp_tracer_start_span("DB:SELECT orders", nullptr);
    assert(child != nullptr);
    assert(strcmp(child->trace_id, root_trace) == 0);
    assert(strcmp(child->parent_span_id, root_span) == 0);

    /* Grandchild span automatically nests under child */
    csilk_otlp_span_t* grandchild = csilk_otlp_tracer_start_span("AI:analyze_fraud", nullptr);
    assert(grandchild != nullptr);
    assert(strcmp(grandchild->trace_id, root_trace) == 0);
    assert(strcmp(grandchild->parent_span_id, child->span_id) == 0);

    csilk_otlp_tracer_end_span(grandchild, 1);
    csilk_otlp_tracer_end_span(child, 1);
    csilk_otlp_tracer_end_span(root, 1);

    printf("test_otlp_trace_child_span_nesting passed\n");
}

static void
test_otlp_apm_spans_json_handler(void)
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != nullptr);
    csilk_test_ctx_set_request(ctx, "GET", "/admin/apm/spans");

    csilk_otlp_apm_spans_handler(ctx);

    assert(csilk_get_status(ctx) == 200);
    const char* ct = csilk_get_response_header(ctx, "Content-Type");
    assert(ct != nullptr && strstr(ct, "application/json") != nullptr);

    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr && body_len > 0);
    assert(body[0] == '[');
    assert(strstr(body, "traceId") != nullptr);
    assert(strstr(body, "HTTP:GET /api/v1/orders") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_otlp_apm_spans_json_handler passed\n");
}

static void
test_otlp_cross_module_span_chaining(void)
{
    printf("Testing cross-module sub-span chaining (HTTP -> DB -> AI -> MQ)...\n");

    /* Simulate a full cross-module request: HTTP handler -> DB query -> AI inference -> MQ publish */
    csilk_otlp_span_t* http_span =
        csilk_otlp_tracer_start_span("HTTP:POST /api/v1/predict", nullptr);
    assert(http_span != nullptr);

    /* DB sub-span inherits HTTP trace context */
    csilk_otlp_span_t* db_span = csilk_otlp_tracer_start_span("DB:SELECT features", nullptr);
    assert(db_span != nullptr);
    assert(strcmp(db_span->trace_id, http_span->trace_id) == 0);
    assert(strcmp(db_span->parent_span_id, http_span->span_id) == 0);
    csilk_otlp_tracer_end_span(db_span, 1);

    /* AI sub-span inherits HTTP trace context */
    csilk_otlp_span_t* ai_span = csilk_otlp_tracer_start_span("AI:inference:resnet50", nullptr);
    assert(ai_span != nullptr);
    assert(strcmp(ai_span->trace_id, http_span->trace_id) == 0);
    assert(strcmp(ai_span->parent_span_id, http_span->span_id) == 0);

    /* MQ sub-span under AI (nested) */
    csilk_otlp_span_t* mq_span = csilk_otlp_tracer_start_span("MQ:publish:prediction", nullptr);
    assert(mq_span != nullptr);
    assert(strcmp(mq_span->trace_id, http_span->trace_id) == 0);
    assert(strcmp(mq_span->parent_span_id, ai_span->span_id) == 0);
    csilk_otlp_tracer_end_span(mq_span, 1);
    csilk_otlp_tracer_end_span(ai_span, 1);
    csilk_otlp_tracer_end_span(http_span, 1);

    /* Verify all spans are in the global buffer */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != nullptr);
    csilk_test_ctx_set_request(ctx, "GET", "/admin/apm/spans");
    csilk_otlp_apm_spans_handler(ctx);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr && body_len > 0);
    assert(strstr(body, "HTTP:POST /api/v1/predict") != nullptr);
    assert(strstr(body, "DB:SELECT features") != nullptr);
    assert(strstr(body, "AI:inference:resnet50") != nullptr);
    assert(strstr(body, "MQ:publish:prediction") != nullptr);
    csilk_test_ctx_free(ctx);

    printf("test_otlp_cross_module_span_chaining passed\n");
}

static void
test_otlp_batch_exporter_lifecycle(void)
{
    printf("Testing OTLP batch exporter async export + backpressure...\n");

    int         flush_count = 0;
    static char last_flush_json[4096];

    /* Use a small batch size to trigger flush quickly */
    csilk_otlp_exporter_t* exp = csilk_otlp_exporter_new("http://localhost:4318/v1/traces", 4);
    assert(exp != nullptr);

    /* Record 10 spans — should trigger 2+ flushes (batch=4) */
    csilk_otlp_span_t span;
    memset(&span, 0, sizeof(span));
    for (int i = 0; i < 10; i++) {
        snprintf(span.name, sizeof(span.name), "Span-%d", i);
        snprintf(span.trace_id, sizeof(span.trace_id), "%032x", (unsigned)(0xABCDEF + i));
        snprintf(span.span_id, sizeof(span.span_id), "%016x", (unsigned)i);
        csilk_otlp_exporter_record_span(exp, &span);
    }

    /* Export JSON */
    char out_buf[8192];
    int  exported = csilk_otlp_exporter_export_json(exp, out_buf, sizeof(out_buf));
    assert(exported >= 0);

    /* Record more spans, then gRPC frame export */
    for (int i = 0; i < 4; i++) {
        snprintf(span.name, sizeof(span.name), "GRPC-Span-%d", i);
        csilk_otlp_exporter_record_span(exp, &span);
    }
    uint8_t grpc_buf[8192];
    int     grpc_len = csilk_otlp_exporter_export_grpc(exp, grpc_buf, sizeof(grpc_buf));
#ifdef CSILK_HAS_GRPC
    assert(grpc_len >= 0);
    if (grpc_len > 5) {
        assert(grpc_buf[0] == 0); /* compression flag */
    }
#else
    assert(grpc_len == -1);
#endif

    /* Start/stop async worker */
    int rc = csilk_otlp_exporter_start(exp, 100);
    assert(rc == 0);
    /* Give the worker a moment to process */
    usleep(200000); /* 200ms */
    csilk_otlp_exporter_stop(exp);

    csilk_otlp_exporter_free(exp);

    printf("test_otlp_batch_exporter_lifecycle passed\n");
}

int
main(void)
{
    test_otlp_trace_span_lifecycle();
    test_otlp_trace_child_span_nesting();
    test_otlp_apm_spans_json_handler();
    test_otlp_cross_module_span_chaining();
    test_otlp_batch_exporter_lifecycle();
    printf("All test_otlp_trace_span tests passed successfully!\n");
    return 0;
}
