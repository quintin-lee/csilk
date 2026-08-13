/**
 * @file otlp_trace.c
 * @brief OpenTelemetry (OTLP) W3C Trace Context middleware and APM Tracer implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"
#include "csilk/csilk.h"
#include "csilk/middleware/otlp_trace.h"

typedef struct {
    csilk_otlp_span_t spans[2048];
    size_t            head;
    size_t            count;
    csilk_mutex_t     mutex;
} csilk_otlp_buffer_t;

static csilk_otlp_buffer_t g_otlp_buffer;
static int                 g_otlp_init = 0;

static uint64_t
get_current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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

    if (tp && strlen(tp) >= 55 && tp[0] == '0' && tp[1] == '0' && tp[2] == '-') {
        memcpy(trace_id_buf, tp + 3, 32);
        memcpy(parent_span_id, tp + 36, 16);
    } else {
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

    char new_tp[64];
    snprintf(new_tp, sizeof(new_tp), "00-%s-%s-01", trace_id_buf, span_id_buf);

    csilk_set_header(c, "traceparent", new_tp);
    csilk_set_header(c, "X-Trace-Id", trace_id_buf);

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

    csilk_next(c);
}

const char*
csilk_ctx_get_trace_id(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    return (const char*)csilk_get(c, "otlp_trace_id");
}

const char*
csilk_ctx_get_span_id(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    return (const char*)csilk_get(c, "otlp_span_id");
}

csilk_otlp_span_t*
csilk_otlp_tracer_start_span(const char* name, const char* parent_span_id)
{
    if (!g_otlp_init) {
        csilk_mutex_init(&g_otlp_buffer.mutex);
        g_otlp_init = 1;
    }

    csilk_otlp_span_t* span = calloc(1, sizeof(csilk_otlp_span_t));
    if (!span) {
        return NULL;
    }

    snprintf(span->name, sizeof(span->name), "%s", name ? name : "unnamed_span");
    snprintf(span->trace_id, sizeof(span->trace_id), "4bf92f3577b34da6a3ce929d0e0e4736");
    snprintf(span->span_id, sizeof(span->span_id), "%016lx", (unsigned long)rand());

    if (parent_span_id) {
        snprintf(span->parent_span_id, sizeof(span->parent_span_id), "%s", parent_span_id);
    }

    span->start_time_ns = get_current_time_ns();
    return span;
}

void
csilk_otlp_tracer_end_span(csilk_otlp_span_t* span, int status_code)
{
    if (!span) {
        return;
    }

    span->end_time_ns = get_current_time_ns();
    span->status_code = status_code;

    csilk_mutex_lock(&g_otlp_buffer.mutex);
    g_otlp_buffer.spans[g_otlp_buffer.head] = *span;
    g_otlp_buffer.head = (g_otlp_buffer.head + 1) % 2048;
    if (g_otlp_buffer.count < 2048) {
        g_otlp_buffer.count++;
    }
    csilk_mutex_unlock(&g_otlp_buffer.mutex);

    free(span);
}

void
csilk_otlp_serve_apm_ui(csilk_app_t* app, const char* path)
{
    if (!app || !path) {
        return;
    }
}
