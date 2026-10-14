#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/json.h"
#include "csilk/drivers/ai.h"

static void
test_trace_init_and_lifecycle(void)
{
    /* 1. Stack initialization */
    csilk_ai_trace_t st;
    csilk_ai_trace_init(&st, 101, 202);
    assert(st.user_id == 101);
    assert(st.session_id == 202);
    assert(strcmp(st.status, "ok") == 0);
    assert(strcmp(st.input_messages, "[]") == 0);
    assert(strcmp(st.output_content, "") == 0);
    assert(strcmp(st.system_prompt, "") == 0);
    assert(strcmp(st.tool_spans, "[]") == 0);
    assert(st.has_first_token == 0);
    csilk_ai_trace_free(&st);

    /* 2. Heap allocation and configuration */
    csilk_ai_trace_t* t = csilk_ai_trace_new(888, 999);
    assert(t != NULL);
    assert(t->user_id == 888);
    assert(t->session_id == 999);

    csilk_ai_trace_set_provider(t, "openai", "gpt-4o");
    assert(strcmp(t->provider, "openai") == 0);
    assert(strcmp(t->model, "gpt-4o") == 0);

    csilk_ai_trace_set_params(t, 0.7, 2048, 0.95);
    assert(fabs(t->temperature - 0.7) < 1e-6);
    assert(t->max_tokens == 2048);
    assert(fabs(t->top_p - 0.95) < 1e-6);

    csilk_ai_trace_set_system_prompt(t, "You are a financial advisor.");
    assert(strcmp(t->system_prompt, "You are a financial advisor.") == 0);

    csilk_ai_trace_free(t);
    free(t);
}

static void
test_trace_token_estimation(void)
{
    /* NULL and empty string */
    assert(csilk_ai_estimate_tokens_from_text(NULL) == 0);
    assert(csilk_ai_estimate_tokens_from_text("") == 0);

    /* ASCII sentence (~50 chars, ~14 tokens) */
    const char* ascii = "Hello world, this is a test prompt for estimating tokens.";
    int         tokens_ascii = csilk_ai_estimate_tokens_from_text(ascii);
    assert(tokens_ascii >= 10 && tokens_ascii <= 25);

    /* CJK sentence (~17 chars, ~20 tokens) */
    const char* cjk = "这是一个用于估算中文字符标记数量的测试用例。";
    int         tokens_cjk = csilk_ai_estimate_tokens_from_text(cjk);
    assert(tokens_cjk >= 15 && tokens_cjk <= 35);

    /* Mixed text */
    const char* mixed = "Hello 世界, 2026 AI Trace 监控与成本估算测试!";
    int         tokens_mixed = csilk_ai_estimate_tokens_from_text(mixed);
    assert(tokens_mixed >= 12 && tokens_mixed <= 40);
}

static void
test_trace_streaming_and_first_token(void)
{
    csilk_ai_trace_t* t = csilk_ai_trace_new(1, 1);
    assert(t != NULL);
    assert(t->has_first_token == 0);
    assert(t->first_token_ms == 0);

    /* Sleep 10ms so monotonic clock advances */
    usleep(10000);

    /* Chunk 1 triggers first token timing */
    csilk_ai_trace_append_output(t, "Hello ");
    assert(t->has_first_token == 1);
    assert(t->first_token_ms >= 0);
    long ft = t->first_token_ms;

    /* Explicit call should be idempotent */
    csilk_ai_trace_record_first_token(t);
    assert(t->first_token_ms == ft);

    /* Subsequent chunks */
    csilk_ai_trace_append_output(t, "world! ");
    csilk_ai_trace_append_output(t, "Welcome to Minefolio.");
    assert(strcmp(t->output_content, "Hello world! Welcome to Minefolio.") == 0);

    csilk_ai_trace_free(t);
    free(t);
}

static void
test_trace_tokens_and_cost_calculation(void)
{
    /* 1. gpt-4o pricing: $2.50 / $10.00 per 1M */
    csilk_ai_trace_t* t1 = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_set_provider(t1, "openai", "gpt-4o");
    csilk_ai_trace_calculate_tokens_and_cost(t1, 1000, 500);
    assert(t1->prompt_tokens == 1000);
    assert(t1->completion_tokens == 500);
    assert(t1->total_tokens == 1500);
    /* 1000 * 2.50/1M + 500 * 10.00/1M = 0.0025 + 0.0050 = 0.0075 */
    assert(fabs(t1->cost_usd - 0.0075) < 1e-7);
    csilk_ai_trace_free(t1);
    free(t1);

    /* 2. gpt-4o-mini pricing: $0.15 / $0.60 per 1M */
    csilk_ai_trace_t* t2 = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_set_provider(t2, "openai", "gpt-4o-mini");
    csilk_ai_trace_calculate_tokens_and_cost(t2, 1000, 1000);
    /* 1000 * 0.15/1M + 1000 * 0.60/1M = 0.00015 + 0.00060 = 0.00075 */
    assert(fabs(t2->cost_usd - 0.00075) < 1e-7);
    csilk_ai_trace_free(t2);
    free(t2);

    /* 3. DeepSeek pricing: $0.14 / $0.28 per 1M */
    csilk_ai_trace_t* t3 = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_set_provider(t3, "deepseek", "deepseek-chat");
    csilk_ai_trace_calculate_tokens_and_cost(t3, 1000, 1000);
    /* 1000 * 0.14/1M + 1000 * 0.28/1M = 0.00014 + 0.00028 = 0.00042 */
    assert(fabs(t3->cost_usd - 0.00042) < 1e-7);
    csilk_ai_trace_free(t3);
    free(t3);

    /* 4. Ollama pricing: $0.0 */
    csilk_ai_trace_t* t4 = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_set_provider(t4, "ollama", "llama3.1:8b");
    csilk_ai_trace_calculate_tokens_and_cost(t4, 2000, 1000);
    assert(t4->cost_usd == 0.0);
    csilk_ai_trace_free(t4);
    free(t4);

    /* 5. Heuristic estimation fallback */
    csilk_ai_trace_t* t5 = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_set_provider(t5, "openai", "gpt-4o");
    csilk_ai_trace_set_system_prompt(t5, "You are a professional investment analyst.");
    csilk_ai_trace_append_output(t5,
                                 "Based on your financial data, your asset allocation is healthy.");
    csilk_ai_trace_calculate_tokens_and_cost(t5, 0, 0);
    assert(t5->prompt_tokens > 0);
    assert(t5->completion_tokens > 0);
    assert(t5->total_tokens == t5->prompt_tokens + t5->completion_tokens);
    assert(t5->cost_usd > 0.0);
    csilk_ai_trace_free(t5);
    free(t5);
}

static void
test_trace_tool_spans(void)
{
    csilk_ai_trace_t* t = csilk_ai_trace_new(1, 1);
    assert(strcmp(t->tool_spans, "[]") == 0);

    /* Append tool spans */
    csilk_ai_trace_add_tool_span(t, "get_stock_quote", 35, 256, 1);
    csilk_ai_trace_add_tool_span(t, "calculate_pnl", 12, 128, 1);
    csilk_ai_trace_add_tool_span(t, "sync_failed", 150, 0, 0);

    /* Verify JSON structure */
    csilk_json_t* arr = csilk_json_parse(t->tool_spans);
    assert(arr != NULL);
    assert(csilk_json_is_array(arr));
    assert(csilk_json_array_size(arr) == 3);

    csilk_json_t* s0 = csilk_json_array_get(arr, 0);
    assert(s0 != NULL);
    csilk_json_t* s0_name = csilk_json_get(s0, "name");
    assert(s0_name != NULL && strcmp(csilk_json_string_value(s0_name), "get_stock_quote") == 0);
    csilk_json_t* s0_lat = csilk_json_get(s0, "latency_ms");
    assert(s0_lat != NULL && csilk_json_as_int64(s0_lat, 0) == 35);
    csilk_json_t* s0_ok = csilk_json_get(s0, "ok");
    assert(s0_ok != NULL && csilk_json_as_int64(s0_ok, 0) == 1);

    csilk_json_t* s2 = csilk_json_array_get(arr, 2);
    assert(s2 != NULL);
    csilk_json_t* s2_name = csilk_json_get(s2, "name");
    assert(s2_name != NULL && strcmp(csilk_json_string_value(s2_name), "sync_failed") == 0);
    csilk_json_t* s2_ok = csilk_json_get(s2, "ok");
    assert(s2_ok != NULL && csilk_json_as_int64(s2_ok, 1) == 0);

    csilk_json_free(arr);
    csilk_ai_trace_free(t);
    free(t);
}

static void
test_trace_finish_and_to_json(void)
{
    csilk_ai_trace_t* t = csilk_ai_trace_new(42, 100);
    csilk_ai_trace_set_provider(t, "openai", "gpt-4o");

    usleep(5000);
    csilk_ai_trace_append_output(t, "Hello, world!");
    csilk_ai_trace_calculate_tokens_and_cost(t, 100, 50);
    csilk_ai_trace_add_tool_span(t, "eval_formula", 15, 64, 1);

    usleep(5000);
    csilk_ai_trace_finish(t, "ok", NULL);

    assert(t->latency_ms >= 0);
    assert(t->tokens_per_sec > 0.0);
    assert(strcmp(t->status, "ok") == 0);

    /* Convert to JSON */
    csilk_json_t* obj = csilk_ai_trace_to_json(t);
    assert(obj != NULL);
    assert(csilk_json_is_object(obj));

    csilk_json_t* uid = csilk_json_get(obj, "user_id");
    assert(uid != NULL && csilk_json_as_int64(uid, 0) == 42);

    csilk_json_t* sid = csilk_json_get(obj, "session_id");
    assert(sid != NULL && csilk_json_as_int64(sid, 0) == 100);

    csilk_json_t* prov = csilk_json_get(obj, "provider");
    assert(prov != NULL && strcmp(csilk_json_string_value(prov), "openai") == 0);

    csilk_json_t* mdl = csilk_json_get(obj, "model");
    assert(mdl != NULL && strcmp(csilk_json_string_value(mdl), "gpt-4o") == 0);

    csilk_json_t* ptok = csilk_json_get(obj, "prompt_tokens");
    assert(ptok != NULL && csilk_json_as_int64(ptok, 0) == 100);

    csilk_json_t* ctok = csilk_json_get(obj, "completion_tokens");
    assert(ctok != NULL && csilk_json_as_int64(ctok, 0) == 50);

    csilk_json_t* spans = csilk_json_get(obj, "tool_spans");
    assert(spans != NULL && csilk_json_is_array(spans));
    assert(csilk_json_array_size(spans) == 1);

    csilk_json_free(obj);
    csilk_ai_trace_free(t);
    free(t);

    /* Error finish case */
    csilk_ai_trace_t* t_err = csilk_ai_trace_new(1, 1);
    csilk_ai_trace_finish(t_err, "error", "Rate limit exceeded");
    assert(strcmp(t_err->status, "error") == 0);
    assert(strcmp(t_err->error_message, "Rate limit exceeded") == 0);
    csilk_ai_trace_free(t_err);
    free(t_err);
}

int
main(void)
{
    printf("Running test_ai_trace suite...\n");
    test_trace_init_and_lifecycle();
    test_trace_token_estimation();
    test_trace_streaming_and_first_token();
    test_trace_tokens_and_cost_calculation();
    test_trace_tool_spans();
    test_trace_finish_and_to_json();
    printf("test_ai_trace: ALL PASSED\n");
    return 0;
}
