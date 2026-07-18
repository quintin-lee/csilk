/**
 * @file otlp_trace.c
 * @brief OpenTelemetry (OTLP) W3C Trace Context middleware implementation.
 */

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/core/middleware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
csilk_trace_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    const char* tp = csilk_get_header(c, "traceparent");
    char        trace_id[33] = {0};
    char        span_id[17] = {0};
    char        parent_span_id[17] = {0};

    if (tp && strlen(tp) >= 55 && tp[0] == '0' && tp[1] == '0' && tp[2] == '-') {
        // Format: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
        memcpy(trace_id, tp + 3, 32);
        memcpy(parent_span_id, tp + 36, 16);
    } else {
        // Generate new 16-byte random trace ID hex
        char uuid_buf[CSILK_UUID_BUF_SIZE];
        csilk_generate_uuid(uuid_buf);
        size_t idx = 0;
        for (size_t i = 0; i < strlen(uuid_buf) && idx < 32; i++) {
            if (uuid_buf[i] != '-') {
                trace_id[idx++] = uuid_buf[i];
            }
        }
        while (idx < 32) {
            trace_id[idx++] = '0';
        }
    }

    // Generate new 8-byte span ID hex
    char span_uuid[CSILK_UUID_BUF_SIZE];
    csilk_generate_uuid(span_uuid);
    size_t s_idx = 0;
    for (size_t i = 0; i < strlen(span_uuid) && s_idx < 16; i++) {
        if (span_uuid[i] != '-') {
            span_id[s_idx++] = span_uuid[i];
        }
    }
    while (s_idx < 16) {
        span_id[s_idx++] = '0';
    }

    char new_tp[64];
    snprintf(new_tp, sizeof(new_tp), "00-%s-%s-01", trace_id, span_id);

    csilk_set_header(c, "traceparent", new_tp);
    csilk_set_header(c, "X-Trace-Id", trace_id);

    csilk_set(c, "otlp_trace_id", trace_id);
    csilk_set(c, "otlp_span_id", span_id);

    csilk_next(c);
}

const char*
csilk_ctx_get_trace_id(csilk_ctx_t* c)
{
    if (!c) {
        return nullptr;
    }
    return (const char*)csilk_get(c, "otlp_trace_id");
}

const char*
csilk_ctx_get_span_id(csilk_ctx_t* c)
{
    if (!c) {
        return nullptr;
    }
    return (const char*)csilk_get(c, "otlp_span_id");
}
