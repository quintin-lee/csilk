/**
 * @file ai_trace.c
 * @brief Full-lifecycle AI Trace monitoring: latency, TTFT, tokens, cost, and tool spans.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/ai.h"
#include "csilk/core/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void
csilk_ai_trace_init(csilk_ai_trace_t* t, int64_t user_id, int64_t session_id)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->user_id = user_id;
    t->session_id = session_id;
    t->input_messages = strdup("[]");
    t->output_content = strdup("");
    t->system_prompt = strdup("");
    t->error_message = strdup("");
    t->metadata = strdup("{}");
    strncpy(t->tool_spans, "[]", sizeof(t->tool_spans) - 1);
    t->tool_spans[sizeof(t->tool_spans) - 1] = '\0';
    strncpy(t->status, "ok", sizeof(t->status) - 1);
    clock_gettime(CLOCK_MONOTONIC, &t->t_start);
}

csilk_ai_trace_t*
csilk_ai_trace_new(int64_t user_id, int64_t session_id)
{
    csilk_ai_trace_t* t = calloc(1, sizeof(csilk_ai_trace_t));
    if (!t) {
        return NULL;
    }
    csilk_ai_trace_init(t, user_id, session_id);
    return t;
}

void
csilk_ai_trace_free(csilk_ai_trace_t* t)
{
    if (!t) {
        return;
    }
    free(t->input_messages);
    free(t->output_content);
    free(t->system_prompt);
    free(t->error_message);
    free(t->metadata);
    t->input_messages = NULL;
    t->output_content = NULL;
    t->system_prompt = NULL;
    t->error_message = NULL;
    t->metadata = NULL;
}

void
csilk_ai_trace_set_provider(csilk_ai_trace_t* t, const char* provider, const char* model)
{
    if (!t) {
        return;
    }
    strncpy(t->provider, provider ? provider : "", sizeof(t->provider) - 1);
    t->provider[sizeof(t->provider) - 1] = '\0';
    strncpy(t->model, model ? model : "", sizeof(t->model) - 1);
    t->model[sizeof(t->model) - 1] = '\0';
}

void
csilk_ai_trace_set_params(csilk_ai_trace_t* t, double temperature, int max_tokens, double top_p)
{
    if (!t) {
        return;
    }
    t->temperature = temperature;
    t->max_tokens = max_tokens;
    t->top_p = top_p;
}

void
csilk_ai_trace_set_system_prompt(csilk_ai_trace_t* t, const char* prompt)
{
    if (!t) {
        return;
    }
    free(t->system_prompt);
    t->system_prompt = strdup(prompt ? prompt : "");
}

void
csilk_ai_trace_append_output(csilk_ai_trace_t* t, const char* chunk)
{
    if (!t || !chunk || !chunk[0]) {
        return;
    }
    if (!t->has_first_token) {
        csilk_ai_trace_record_first_token(t);
    }
    size_t clen = strlen(chunk);
    size_t olen = t->output_content ? strlen(t->output_content) : 0;
    size_t nlen = olen + clen + 1;
    char*  buf = realloc(t->output_content, nlen);
    if (!buf) {
        return;
    }
    memcpy(buf + olen, chunk, clen);
    buf[olen + clen] = '\0';
    t->output_content = buf;
    t->accumulated_len = (long)nlen;
}

void
csilk_ai_trace_record_first_token(csilk_ai_trace_t* t)
{
    if (!t || t->has_first_token) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t->t_first_token);
    t->has_first_token = 1;
    t->first_token_ms = (t->t_first_token.tv_sec - t->t_start.tv_sec) * 1000 +
                        (t->t_first_token.tv_nsec - t->t_start.tv_nsec) / 1000000;
    if (t->first_token_ms < 0) {
        t->first_token_ms = 0;
    }
}

int
csilk_ai_estimate_tokens_from_text(const char* text)
{
    if (!text || !*text) {
        return 0;
    }
    size_t char_count = 0;
    size_t ascii_count = 0;
    size_t cjk_count = 0;
    size_t i = 0;

    while (text[i] != '\0') {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0x80) == 0) {
            ascii_count++;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cjk_count++;
            i += (text[i + 1] != '\0') ? 2 : 1;
        } else if ((c & 0xF0) == 0xE0) {
            cjk_count++;
            i += (text[i + 1] != '\0' && text[i + 2] != '\0') ? 3 : 1;
        } else if ((c & 0xF8) == 0xF0) {
            cjk_count++;
            i += (text[i + 1] != '\0' && text[i + 2] != '\0' && text[i + 3] != '\0') ? 4 : 1;
        } else {
            ascii_count++;
            i += 1;
        }
        char_count++;
    }

    int estimated = (int)((double)ascii_count / 3.5 + (double)cjk_count * 1.2);
    if (estimated <= 0 && char_count > 0) {
        estimated = 1;
    }
    return estimated;
}

void
csilk_ai_trace_calculate_tokens_and_cost(csilk_ai_trace_t* t,
                                         int               prompt_tokens,
                                         int               completion_tokens)
{
    if (!t) {
        return;
    }

    if (prompt_tokens > 0) {
        t->prompt_tokens = prompt_tokens;
    } else {
        int est_in = csilk_ai_estimate_tokens_from_text(t->input_messages);
        int est_sys = csilk_ai_estimate_tokens_from_text(t->system_prompt);
        t->prompt_tokens = est_in + est_sys + 8;
    }

    if (completion_tokens > 0) {
        t->completion_tokens = completion_tokens;
    } else {
        t->completion_tokens = csilk_ai_estimate_tokens_from_text(t->output_content);
    }

    t->total_tokens = t->prompt_tokens + t->completion_tokens;

    /* Pricing per 1M tokens in USD */
    double in_price_per_1m = 0.50;
    double out_price_per_1m = 1.50;

    if (strstr(t->model, "gpt-4o-mini") || strstr(t->model, "4o-mini")) {
        in_price_per_1m = 0.15;
        out_price_per_1m = 0.60;
    } else if (strstr(t->model, "gpt-4o") || strstr(t->model, "gpt-4")) {
        in_price_per_1m = 2.50;
        out_price_per_1m = 10.00;
    } else if (strstr(t->provider, "deepseek") || strstr(t->model, "deepseek")) {
        in_price_per_1m = 0.14;
        out_price_per_1m = 0.28;
    } else if (strstr(t->provider, "qwen") || strstr(t->model, "qwen")) {
        in_price_per_1m = 0.20;
        out_price_per_1m = 0.60;
    } else if (strstr(t->provider, "ollama")) {
        in_price_per_1m = 0.0;
        out_price_per_1m = 0.0;
    }

    t->cost_usd = ((double)t->prompt_tokens * in_price_per_1m / 1000000.0) +
                  ((double)t->completion_tokens * out_price_per_1m / 1000000.0);
}

void
csilk_ai_trace_add_tool_span(
    csilk_ai_trace_t* t, const char* name, long latency_ms, size_t bytes, int ok)
{
    if (!t || !name) {
        return;
    }
    size_t cur = strlen(t->tool_spans);
    if (cur + 256 >= sizeof(t->tool_spans)) {
        return;
    }
    char entry[256];
    int  n = snprintf(entry,
                      sizeof(entry),
                      "{\"name\":\"%s\",\"latency_ms\":%ld,\"bytes\":%zu,\"ok\":%d}",
                      name,
                      latency_ms,
                      bytes,
                      ok ? 1 : 0);
    if (n < 0) {
        return;
    }
    const char* sep = (cur <= 2) ? "" : ",";
    size_t      pos = cur > 0 ? cur - 1 : 0;
    if (t->tool_spans[pos] == ']') {
        t->tool_spans[pos] = '\0';
    }
    snprintf(t->tool_spans + strlen(t->tool_spans),
             sizeof(t->tool_spans) - strlen(t->tool_spans),
             "%s%s]",
             sep,
             entry);
}

void
csilk_ai_trace_finish(csilk_ai_trace_t* t, const char* status, const char* error)
{
    if (!t) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t->t_end);
    strncpy(t->status, status ? status : "ok", sizeof(t->status) - 1);
    t->status[sizeof(t->status) - 1] = '\0';

    if (error && error[0]) {
        free(t->error_message);
        t->error_message = strdup(error);
    }

    t->latency_ms = (t->t_end.tv_sec - t->t_start.tv_sec) * 1000 +
                    (t->t_end.tv_nsec - t->t_start.tv_nsec) / 1000000;
    if (t->latency_ms < 0) {
        t->latency_ms = 0;
    }

    /* Compute generation throughput (tokens/s) */
    if (t->completion_tokens > 0) {
        long gen_ms = (t->first_token_ms > 0 && t->latency_ms > t->first_token_ms)
                          ? (t->latency_ms - t->first_token_ms)
                          : t->latency_ms;
        if (gen_ms > 0) {
            t->tokens_per_sec = (double)t->completion_tokens / ((double)gen_ms / 1000.0);
        } else {
            t->tokens_per_sec = (double)t->completion_tokens;
        }
    } else if (t->total_tokens > 0 && t->latency_ms > 0) {
        t->tokens_per_sec = (double)t->total_tokens / ((double)t->latency_ms / 1000.0);
    }
}

csilk_json_t*
csilk_ai_trace_to_json(const csilk_ai_trace_t* t)
{
    if (!t) {
        return NULL;
    }
    csilk_json_t* obj = csilk_json_object();
    if (!obj) {
        return NULL;
    }

    csilk_json_add_int(obj, "user_id", t->user_id);
    csilk_json_add_int(obj, "session_id", t->session_id);
    csilk_json_add_string(obj, "provider", t->provider);
    csilk_json_add_string(obj, "model", t->model);
    csilk_json_add_int(obj, "prompt_tokens", t->prompt_tokens);
    csilk_json_add_int(obj, "completion_tokens", t->completion_tokens);
    csilk_json_add_int(obj, "total_tokens", t->total_tokens);
    csilk_json_add_int(obj, "latency_ms", t->latency_ms);
    csilk_json_add_int(obj, "first_token_ms", t->first_token_ms);
    csilk_json_add_number(obj, "tokens_per_sec", t->tokens_per_sec);
    csilk_json_add_number(obj, "cost_usd", t->cost_usd);
    csilk_json_add_string(obj, "status", t->status);

    csilk_json_t* spans = csilk_json_parse(t->tool_spans[0] ? t->tool_spans : "[]");
    if (spans) {
        csilk_json_add_object(obj, "tool_spans", spans);
    }

    return obj;
}
