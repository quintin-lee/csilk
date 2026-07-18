/**
 * @file otlp_trace.c
 * @brief OpenTelemetry (OTLP) W3C Trace Context middleware implementation.
 *
 * Implements W3C Trace Context Level 1 specification parsing and propagation:
 * - Parses incoming `traceparent` HTTP headers (`00-<32hex trace_id>-<16hex parent_span_id>-01`).
 * - Generates cryptographically pseudorandom 128-bit Trace ID and 64-bit Span ID if omitted.
 * - Injects updated `traceparent` and `X-Trace-Id` headers into request/response contexts.
 * - Stores Trace ID and Span ID into arena-backed context storage for downstream middleware.
 */

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/core/middleware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief OpenTelemetry W3C Trace Context middleware.
 *
 * @param c Request context handle.
 */
void
csilk_trace_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    const char* tp = csilk_get_header(c, "traceparent");
    char        trace_id_buf[33] = {0};
    char        span_id_buf[17] = {0};
    char        parent_span_id[17] = {0};

    /* Check if valid W3C traceparent header is present (version 00) */
    if (tp && strlen(tp) >= 55 && tp[0] == '0' && tp[1] == '0' && tp[2] == '-') {
        /* Format: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01 */
        memcpy(trace_id_buf, tp + 3, 32);
        memcpy(parent_span_id, tp + 36, 16);
    } else {
        /* Generate a new 128-bit (32 hex char) Trace ID from RFC 4122 UUID */
        char uuid_buf[CSILK_UUID_BUF_SIZE];
        csilk_generate_uuid(uuid_buf);
        size_t idx = 0;
        for (size_t i = 0; i < strlen(uuid_buf) && idx < 32; i++) {
            if (uuid_buf[i] != '-') {
                trace_id_buf[idx++] = uuid_buf[i];
            }
        }
        while (idx < 32) {
            trace_id_buf[idx++] = '0';
        }
    }

    /* Generate a new 64-bit (16 hex char) Span ID for current processing node */
    char span_uuid[CSILK_UUID_BUF_SIZE];
    csilk_generate_uuid(span_uuid);
    size_t s_idx = 0;
    for (size_t i = 0; i < strlen(span_uuid) && s_idx < 16; i++) {
        if (span_uuid[i] != '-') {
            span_id_buf[s_idx++] = span_uuid[i];
        }
    }
    while (s_idx < 16) {
        span_id_buf[s_idx++] = '0';
    }

    /* Construct outgoing W3C traceparent header string */
    char new_tp[64];
    snprintf(new_tp, sizeof(new_tp), "00-%s-%s-01", trace_id_buf, span_id_buf);

    /* Inject HTTP headers into request/response context */
    csilk_set_header(c, "traceparent", new_tp);
    csilk_set_header(c, "X-Trace-Id", trace_id_buf);

    /* Allocate Trace ID and Span ID strings in request Arena memory pool to avoid dangling pointers */
    csilk_arena_t* arena = csilk_get_arena(c);
    if (arena) {
        char* stored_trace_id = csilk_arena_strdup(arena, trace_id_buf);
        char* stored_span_id = csilk_arena_strdup(arena, span_id_buf);
        csilk_set(c, "otlp_trace_id", stored_trace_id);
        csilk_set(c, "otlp_span_id", stored_span_id);
    } else {
        csilk_set(c, "otlp_trace_id", (void*)trace_id_buf);
        csilk_set(c, "otlp_span_id", (void*)span_id_buf);
    }

    /* Pass control to next middleware in chain */
    csilk_next(c);
}

/**
 * @brief Retrieve current Trace ID from request context.
 *
 * @param c Request context.
 * @return 32-character hex Trace ID, or nullptr if unavailable.
 */
const char*
csilk_ctx_get_trace_id(csilk_ctx_t* c)
{
    if (!c) {
        return nullptr;
    }
    return (const char*)csilk_get(c, "otlp_trace_id");
}

/**
 * @brief Retrieve current Span ID from request context.
 *
 * @param c Request context.
 * @return 16-character hex Span ID, or nullptr if unavailable.
 */
const char*
csilk_ctx_get_span_id(csilk_ctx_t* c)
{
    if (!c) {
        return nullptr;
    }
    return (const char*)csilk_get(c, "otlp_span_id");
}
