/**
 * @file otlp_trace.c
 * @brief OpenTelemetry (OTLP) W3C Trace Context middleware and APM Tracer implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/rand.h>
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

typedef struct {
    char trace_id[33];
    char span_id[17];
} csilk_trace_ctx_node_t;

#define CSILK_MAX_TRACE_DEPTH 16
static _Thread_local csilk_trace_ctx_node_t tls_trace_stack[CSILK_MAX_TRACE_DEPTH];
static _Thread_local int                    tls_trace_depth = 0;

static void
_csilk_generate_hex_id(char* out, size_t num_bytes)
{
    uint8_t bytes[16];
    if (num_bytes > sizeof(bytes)) {
        num_bytes = sizeof(bytes);
    }
    if (RAND_bytes(bytes, (int)num_bytes) != 1) {
        FILE* fp = fopen("/dev/urandom", "rb");
        if (fp && fread(bytes, 1, num_bytes, fp) == num_bytes) {
            fclose(fp);
        } else {
            if (fp) {
                fclose(fp);
            }
            char uuid_buf[CSILK_UUID_BUF_SIZE];
            csilk_generate_uuid(uuid_buf);
            size_t idx = 0;
            for (size_t i = 0; i < strlen(uuid_buf) && idx < num_bytes; i++) {
                if (uuid_buf[i] != '-') {
                    bytes[idx++] = (uint8_t)uuid_buf[i];
                }
            }
            while (idx < num_bytes) {
                bytes[idx++] = (uint8_t)rand();
            }
        }
    }
    for (size_t i = 0; i < num_bytes; i++) {
        sprintf(out + (i * 2), "%02x", bytes[i]);
    }
    out[num_bytes * 2] = '\0';
}

void
csilk_otlp_push_trace_context(const char* trace_id, const char* span_id)
{
    if (tls_trace_depth < CSILK_MAX_TRACE_DEPTH) {
        if (trace_id && trace_id[0] != '\0') {
            snprintf(tls_trace_stack[tls_trace_depth].trace_id,
                     sizeof(tls_trace_stack[tls_trace_depth].trace_id),
                     "%s",
                     trace_id);
        } else {
            tls_trace_stack[tls_trace_depth].trace_id[0] = '\0';
        }
        if (span_id && span_id[0] != '\0') {
            snprintf(tls_trace_stack[tls_trace_depth].span_id,
                     sizeof(tls_trace_stack[tls_trace_depth].span_id),
                     "%s",
                     span_id);
        } else {
            tls_trace_stack[tls_trace_depth].span_id[0] = '\0';
        }
        tls_trace_depth++;
    }
}

void
csilk_otlp_pop_trace_context(void)
{
    if (tls_trace_depth > 0) {
        tls_trace_depth--;
    }
}

int
csilk_otlp_get_current_trace_context(char*  out_trace_id,
                                     size_t tid_sz,
                                     char*  out_span_id,
                                     size_t sid_sz)
{
    if (tls_trace_depth <= 0) {
        return -1;
    }
    if (out_trace_id && tid_sz > 0) {
        snprintf(out_trace_id, tid_sz, "%s", tls_trace_stack[tls_trace_depth - 1].trace_id);
    }
    if (out_span_id && sid_sz > 0) {
        snprintf(out_span_id, sid_sz, "%s", tls_trace_stack[tls_trace_depth - 1].span_id);
    }
    return 0;
}

/** @brief Return the current monotonic clock time in nanoseconds. */
static uint64_t
get_current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief W3C Trace Context propagation middleware.
 *
 * Parses the incoming "traceparent" header (W3C format) to resume an existing
 * trace, or synthesizes a new 32-hex-char trace ID and a 16-hex-char span ID
 * from UUIDs when none is present. It sets the "traceparent" and "X-Trace-Id"
 * response headers and stores the trace/span IDs in the context for downstream
 * handlers, then calls csilk_next().
 *
 * @param c  The request context. If NULL the function returns immediately.
 */
void
csilk_trace_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    if (!g_otlp_init) {
        csilk_mutex_init(&g_otlp_buffer.mutex);
        csilk_tracer_hooks_t hooks = {
            .start_span = (void* (*)(const char*, const char*))csilk_otlp_tracer_start_span,
            .end_span = (void (*)(void*, int))csilk_otlp_tracer_end_span,
        };
        csilk_tracer_set_hooks(&hooks);
        g_otlp_init = 1;
    }

    const char* tp = csilk_get_header(c, "traceparent");
    char        trace_id_buf[33] = {0};
    char        span_id_buf[17] = {0};
    char        parent_span_id[17] = {0};

    if (tp && strlen(tp) >= 55 && tp[0] == '0' && tp[1] == '0' && tp[2] == '-') {
        memcpy(trace_id_buf, tp + 3, 32);
        memcpy(parent_span_id, tp + 36, 16);
    } else {
        _csilk_generate_hex_id(trace_id_buf, 16);
    }

    _csilk_generate_hex_id(span_id_buf, 8);

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

    /* Push request trace context so child spans automatically inherit trace ID */
    csilk_otlp_push_trace_context(trace_id_buf, parent_span_id[0] ? parent_span_id : NULL);
    const char*        req_path = csilk_get_path(c);
    csilk_otlp_span_t* root_span = csilk_otlp_tracer_start_span(
        req_path ? req_path : "HTTP Request", parent_span_id[0] ? parent_span_id : NULL);

    csilk_next(c);

    int status = csilk_get_status(c);
    int status_code = (status >= 400 || status == 0) ? 2 : 1;
    csilk_otlp_tracer_end_span(root_span, status_code);
    csilk_otlp_pop_trace_context();
}

/**
 * @brief Retrieve the current request's trace ID.
 *
 * Returns the trace ID previously stored by csilk_trace_middleware() under the
 * "otlp_trace_id" context key.
 *
 * @param c  The request context. Must not be NULL.
 * @return The null-terminated trace ID string, or NULL if not set.
 */
const char*
csilk_ctx_get_trace_id(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    return (const char*)csilk_get(c, "otlp_trace_id");
}

/**
 * @brief Retrieve the current request's span ID.
 *
 * Returns the span ID previously stored by csilk_trace_middleware() under the
 * "otlp_span_id" context key.
 *
 * @param c  The request context. Must not be NULL.
 * @return The null-terminated span ID string, or NULL if not set.
 */
const char*
csilk_ctx_get_span_id(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    return (const char*)csilk_get(c, "otlp_span_id");
}

/**
 * @brief Create and start a new tracing span for the APM tracer.
 *
 * Allocates a csilk_otlp_span_t, assigns the span name, a fixed demo trace ID,
 * a randomized span ID, and an optional parent span ID, and records the start
 * timestamp. The buffer mutex is lazily initialized on first use.
 *
 * @param name             Human-readable span name (defaults to "unnamed_span"
 *                         if NULL).
 * @param parent_span_id   Optional parent span ID string (may be NULL).
 *
 * @return Pointer to the newly allocated span (caller passes to
 *         csilk_otlp_tracer_end_span()), or NULL on allocation failure.
 */
csilk_otlp_span_t*
csilk_otlp_tracer_start_span(const char* name, const char* parent_span_id)
{
    if (!g_otlp_init) {
        csilk_mutex_init(&g_otlp_buffer.mutex);
        csilk_tracer_hooks_t hooks = {
            .start_span = (void* (*)(const char*, const char*))csilk_otlp_tracer_start_span,
            .end_span = (void (*)(void*, int))csilk_otlp_tracer_end_span,
        };
        csilk_tracer_set_hooks(&hooks);
        g_otlp_init = 1;
    }

    csilk_otlp_span_t* span = calloc(1, sizeof(csilk_otlp_span_t));
    if (!span) {
        return NULL;
    }

    snprintf(span->name, sizeof(span->name), "%s", name ? name : "unnamed_span");

    /* Determine trace_id and parent_span_id */
    if (tls_trace_depth > 0 && tls_trace_stack[tls_trace_depth - 1].trace_id[0] != '\0') {
        snprintf(span->trace_id,
                 sizeof(span->trace_id),
                 "%s",
                 tls_trace_stack[tls_trace_depth - 1].trace_id);
        if (parent_span_id && parent_span_id[0] != '\0') {
            snprintf(span->parent_span_id, sizeof(span->parent_span_id), "%s", parent_span_id);
        } else if (tls_trace_stack[tls_trace_depth - 1].span_id[0] != '\0') {
            snprintf(span->parent_span_id,
                     sizeof(span->parent_span_id),
                     "%s",
                     tls_trace_stack[tls_trace_depth - 1].span_id);
        }
    } else {
        _csilk_generate_hex_id(span->trace_id, 16);
        if (parent_span_id && parent_span_id[0] != '\0') {
            snprintf(span->parent_span_id, sizeof(span->parent_span_id), "%s", parent_span_id);
        }
    }

    _csilk_generate_hex_id(span->span_id, 8);

    /* Push this span onto thread trace stack so downstream sub-spans nest automatically */
    csilk_otlp_push_trace_context(span->trace_id, span->span_id);

    span->start_time_ns = get_current_time_ns();
    return span;
}

csilk_otlp_span_t*
csilk_otlp_tracer_start_span_ctx(csilk_ctx_t* c, const char* name)
{
    if (c) {
        const char* tid = csilk_ctx_get_trace_id(c);
        const char* sid = csilk_ctx_get_span_id(c);
        if (tid && tid[0] != '\0') {
            csilk_otlp_push_trace_context(tid, sid);
            csilk_otlp_span_t* s = csilk_otlp_tracer_start_span(name, sid);
            if (tls_trace_depth >= 2) {
                tls_trace_stack[tls_trace_depth - 2] = tls_trace_stack[tls_trace_depth - 1];
                tls_trace_depth--;
            }
            return s;
        }
    }
    return csilk_otlp_tracer_start_span(name, NULL);
}

/**
 * @brief Finalize a span and append it to the global trace buffer.
 *
 * Records the end timestamp and status code, copies the span into the
 * ring-buffer of completed spans (g_otlp_buffer) under the buffer mutex, then
 * frees the caller's span object.
 *
 * @param[in,out] span        The span returned by
 *                            csilk_otlp_tracer_start_span(). If NULL, returns
 *                            immediately.
 * @param[in]     status_code Final span status code.
 */
void
csilk_otlp_tracer_end_span(csilk_otlp_span_t* span, int status_code)
{
    if (!span) {
        return;
    }

    span->end_time_ns = get_current_time_ns();
    span->status_code = status_code;

    if (tls_trace_depth > 0 &&
        strcmp(tls_trace_stack[tls_trace_depth - 1].span_id, span->span_id) == 0) {
        tls_trace_depth--;
    }

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
csilk_otlp_apm_spans_handler(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_mutex_lock(&g_otlp_buffer.mutex);
    csilk_json_t* arr = csilk_json_array();
    size_t        count = g_otlp_buffer.count;
    size_t        start = (g_otlp_buffer.head + 2048 - count) % 2048;
    for (size_t i = 0; i < count; i++) {
        size_t                   idx = (start + i) % 2048;
        const csilk_otlp_span_t* s = &g_otlp_buffer.spans[idx];
        csilk_json_t*            obj = csilk_json_object();
        csilk_json_add_string(obj, "traceId", s->trace_id);
        csilk_json_add_string(obj, "spanId", s->span_id);
        if (s->parent_span_id[0] != '\0') {
            csilk_json_add_string(obj, "parentSpanId", s->parent_span_id);
        }
        csilk_json_add_string(obj, "name", s->name);
        csilk_json_add_number(obj, "startTimeUnixNano", (double)s->start_time_ns);
        csilk_json_add_number(obj, "endTimeUnixNano", (double)s->end_time_ns);
        double dur_ms = (s->end_time_ns >= s->start_time_ns)
                            ? (double)(s->end_time_ns - s->start_time_ns) / 1000000.0
                            : 0.0;
        csilk_json_add_number(obj, "durationMs", dur_ms);
        csilk_json_add_number(obj, "statusCode", s->status_code);
        csilk_json_array_append(arr, obj);
    }
    csilk_mutex_unlock(&g_otlp_buffer.mutex);

    char* json_str = csilk_json_serialize(arr, NULL);
    csilk_json_free(arr);
    if (!json_str) {
        csilk_json_error(c, 500, "failed to serialize telemetry spans");
        return;
    }
    csilk_set_header(c, "Content-Type", "application/json");
    csilk_string(c, 200, json_str);
    free(json_str);
}

void
csilk_otlp_apm_ui_handler(csilk_ctx_t* c)
{
    const char* paths[] = {"share/csilk/apm_ui.html",
                           "../share/csilk/apm_ui.html",
                           "/usr/local/share/csilk/apm_ui.html"};

    for (int i = 0; i < 3; i++) {
        FILE* f = fopen(paths[i], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                char* buf = calloc(1, (size_t)sz + 1);
                if (buf) {
                    if (fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                        csilk_set_header(c, "Content-Type", "text/html");
                        csilk_string(c, 200, buf);
                    }
                    free(buf);
                }
            }
            fclose(f);
            return;
        }
    }

    /* Embedded fallback if template file is not on disk */
    static const char g_embedded_apm_ui_html[] =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head><title>csilk APM Tracing</title></head>\n"
        "<body><h1>⚡ csilk APM & eBPF Control Plane</h1>\n"
        "<p>Real-Time OpenTelemetry Distributed Tracing Waterfall & Kernel Drop Counter</p>\n"
        "</body></html>\n";

    csilk_set_header(c, "Content-Type", "text/html");
    csilk_string(c, 200, g_embedded_apm_ui_html);
}

/**
 * @brief Register an APM UI route to serve a trace explorer page.
 *
 * Mounts the trace visualization APM dashboard endpoint at the given path on
 * the application.
 *
 * @param app   The csilk application to register the route on. Must not be NULL.
 * @param path  URL path for the APM UI endpoint. Must not be NULL.
 */
void
csilk_otlp_serve_apm_ui(csilk_app_t* app, const char* path)
{
    if (!app || !path) {
        return;
    }
    csilk_app_add_route(app, "GET", path, csilk_otlp_apm_ui_handler);

    char   spans_path[256];
    size_t path_len = strlen(path);
    while (path_len > 0 && path[path_len - 1] == '/') {
        path_len--;
    }
    snprintf(spans_path, sizeof(spans_path), "%.*s/spans", (int)path_len, path);
    csilk_app_add_route(app, "GET", spans_path, csilk_otlp_apm_spans_handler);
    csilk_app_add_route(app, "GET", "/admin/api/telemetry/spans", csilk_otlp_apm_spans_handler);
}
