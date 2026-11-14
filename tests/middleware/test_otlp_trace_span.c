#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int
main(void)
{
    test_otlp_trace_span_lifecycle();
    test_otlp_trace_child_span_nesting();
    test_otlp_apm_spans_json_handler();
    printf("All test_otlp_trace_span tests passed successfully!\n");
    return 0;
}
