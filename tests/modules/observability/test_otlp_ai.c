/**
 * @file test_otlp_ai.c
 * @brief Unit test for OpenTelemetry 2.0 AI Trace ↔ HTTP Trace correlation.
 */

#include "csilk/csilk.h"
#include "csilk/observability/otlp_trace.h"
#include "csilk/drivers/ai.h"
#include "csilk/test/test.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    printf("=== Test: OpenTelemetry AI Trace Correlation ===\n");

    csilk_otlp_span_t* span = csilk_otlp_tracer_start_span("HTTP POST /api/chat", NULL);
    assert(span != NULL);
    assert(span->span_id[0] != '\0');
    assert(span->trace_id[0] != '\0');

    /* Create AI trace representation */
    csilk_ai_trace_t* ai = csilk_ai_trace_new(1001, 2002);
    assert(ai != NULL);
    csilk_ai_trace_set_provider(ai, "openai", "gpt-4o");
    ai->prompt_tokens = 42;
    ai->completion_tokens = 18;
    ai->total_tokens = 60;
    ai->first_token_ms = 120;
    ai->latency_ms = 350;
    ai->cost_usd = 0.0012;

    /* Add tool span info to JSON */
    strncpy(ai->tool_spans,
            "[{\"name\":\"calculator\",\"latency_ms\":15.0,\"status\":\"ok\"}]",
            sizeof(ai->tool_spans) - 1);

    /* Bind AI trace to OTLP span */
    int rc = csilk_otlp_span_bind_ai_trace(span, ai);
    assert(rc == 0);

    /* Export span as JSON to verify attributes serialization */
    char   json_buf[4096] = {0};
    size_t out_len = 0;
    int    exp_rc = csilk_otlp_export_span_json(span, json_buf, sizeof(json_buf), &out_len);
    assert(exp_rc == 0);
    assert(out_len > 0);

    /* Verify standard OpenTelemetry semantic conventions for GenAI */
    assert(strstr(json_buf, "gen_ai.usage.prompt_tokens") != NULL);
    assert(strstr(json_buf, "42") != NULL);
    assert(strstr(json_buf, "gen_ai.usage.completion_tokens") != NULL);
    assert(strstr(json_buf, "18") != NULL);
    assert(strstr(json_buf, "gen_ai.usage.total_tokens") != NULL);
    assert(strstr(json_buf, "60") != NULL);
    assert(strstr(json_buf, "gen_ai.client.ttft_ms") != NULL);
    assert(strstr(json_buf, "gen_ai.response.model") != NULL);
    assert(strstr(json_buf, "gpt-4o") != NULL);

    /* End the span */
    csilk_otlp_tracer_end_span(span, 1);

    /* Context attachment test */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);
    csilk_otlp_span_t* ctx_span = csilk_otlp_tracer_start_span_ctx(ctx, "HTTP Context Span");
    assert(ctx_span != NULL);

    int attach_rc = csilk_otlp_ctx_attach_ai_trace(ctx, ai);
    assert(attach_rc == 0);

    csilk_otlp_tracer_end_span(ctx_span, 1);
    csilk_test_ctx_free(ctx);
    csilk_ai_trace_free(ai);

    printf("PASS: test_otlp_ai\n");
    return 0;
}
