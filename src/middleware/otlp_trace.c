/**
 * @file otlp_trace.c
 * @brief OpenTelemetry (OTLP) W3C Trace Context middleware and APM Tracer implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/rand.h>
#include "csilk/core/internal.h"
#include "csilk/core/json/json.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"
#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"
#include "csilk/middleware/otlp_trace.h"
#include "core/internal/srv_internal.h"

#define OTLP_SPAN_BUF_CAP 2048 /* default; overridable via csilk_otlp_tracer_set_buffer_size */
#define OTLP_SAMPLE_RATE_DEFAULT 1000 /* sample 1/1000 when buffer is full */
#define CSILK_MAX_TRACE_DEPTH 16
#define CSILK_OTLP_SIDECAR_CAP 1024

static void _csilk_otlp_state_free(csilk_server_t* server);

typedef struct {
    char trace_id[33];
    char span_id[17];
} csilk_trace_ctx_node_t;

typedef struct {
    char          span_id[17];
    csilk_json_t* attributes;
    bool          in_use;
} csilk_otlp_sidecar_entry_t;

/**
 * @brief Converged OTLP tracer state.
 *
 * Holds every piece of process-level OTLP tracer state that was previously
 * scattered as independent statics (the span ring buffer, sample rate, total
 * span counter, and the sidecar attribute table). Lazily allocated once via
 * g_otlp_state; the pointer persists for the process lifetime. The
 * thread-local trace stack is intentionally kept separate (per-thread state
 * that cannot be shared across threads).
 */
typedef struct {
    csilk_once_t               init_once;
    csilk_otlp_span_t*         spans; /* span ring buffer */
    size_t                     cap;   /* capacity of spans array */
    size_t                     head;
    size_t                     count;
    csilk_mutex_t              buffer_mutex;
    _Atomic int                sample_rate;
    _Atomic uint64_t           total_spans; /* total spans received (for sampling decision) */
    csilk_otlp_sidecar_entry_t sidecar_table[CSILK_OTLP_SIDECAR_CAP];
    csilk_mutex_t              sidecar_mutex;
} otlp_state_t;

/** @brief Single global pointer to the lazily-initialized OTLP tracer state. */
static otlp_state_t* g_otlp_state = NULL;

static _Thread_local csilk_trace_ctx_node_t tls_trace_stack[CSILK_MAX_TRACE_DEPTH];
static _Thread_local int                    tls_trace_depth = 0;

/* Forward declarations for the scoped hook implementations registered below. */
static void* _otlp_hook_start_span(const char* name, const char* parent_span_id, void* scope);
static void  _otlp_hook_end_span(void* span, int status_code, void* scope);

static void
_otlp_init_once_cb(void)
{
    otlp_state_t* st = g_otlp_state;
    csilk_mutex_init(&st->buffer_mutex);
    size_t cap = st->cap > 0 ? st->cap : OTLP_SPAN_BUF_CAP;
    st->cap = cap;
    st->spans = calloc(cap, sizeof(csilk_otlp_span_t));
    csilk_mutex_init(&st->sidecar_mutex);

    csilk_tracer_hooks_t hooks = {
        .start_span = _otlp_hook_start_span,
        .end_span = _otlp_hook_end_span,
    };
    csilk_tracer_set_hooks(&hooks);
}

/**
 * @brief Lazily obtain the global OTLP tracer state, allocating and
 * initializing it on first use. The state persists for the process lifetime.
 *
 * @return The global OTLP tracer state (never NULL on success).
 */
static otlp_state_t*
_otlp_state_ensure(void)
{
    if (!g_otlp_state) {
        otlp_state_t* st = calloc(1, sizeof(otlp_state_t));
        if (!st) {
            return NULL;
        }
        st->cap = OTLP_SPAN_BUF_CAP;
        atomic_init(&st->sample_rate, OTLP_SAMPLE_RATE_DEFAULT);
        atomic_init(&st->total_spans, 0);
        csilk_once_init(&st->init_once);
        g_otlp_state = st;
    }
    csilk_once(&g_otlp_state->init_once, _otlp_init_once_cb);
    return g_otlp_state;
}

/**
 * @brief Lazily obtain the per-server OTLP tracer state. The state is owned
 * by the server and freed by _csilk_otlp_state_free() at teardown. Hook
 * registration remains process-level (see _otlp_init_once_cb); this only
 * allocates an independent span ring buffer + sidecar table for the server.
 */
static otlp_state_t*
_otlp_state_ensure_server(csilk_server_t* server)
{
    otlp_state_t* st = (otlp_state_t*)server->otlp_state;
    if (!st) {
        st = calloc(1, sizeof(otlp_state_t));
        if (!st) {
            return NULL;
        }
        st->cap = OTLP_SPAN_BUF_CAP;
        atomic_init(&st->sample_rate, OTLP_SAMPLE_RATE_DEFAULT);
        atomic_init(&st->total_spans, 0);
        csilk_once_init(&st->init_once);
        server->otlp_state = st;
        _csilk_server_register_teardown(server, _csilk_otlp_state_free);
    }
    /* Initialize this server's buffers (buffer/sidecar only; hooks stay global). */
    csilk_mutex_init(&st->buffer_mutex);
    csilk_mutex_init(&st->sidecar_mutex);
    size_t cap = st->cap > 0 ? st->cap : OTLP_SPAN_BUF_CAP;
    st->spans = calloc(cap, sizeof(csilk_otlp_span_t));
    st->cap = cap;
    return st;
}

/**
 * @brief Resolve the OTLP tracer state for a request context.
 *
 * Prefers the per-server state when the context is bound to a server, falling
 * back to the process-level global state for server-less contexts (tests).
 */
static otlp_state_t*
_otlp_state_for_ctx(csilk_ctx_t* c)
{
    csilk_server_t* server = csilk_ctx_get_server(c);
    if (server) {
        otlp_state_t* st = _otlp_state_ensure_server(server);
        if (st) {
            return st;
        }
    }
    return _otlp_state_ensure();
}

/**
 * @brief Free a server's per-server OTLP tracer state. Called from
 * csilk_server_free(). Frees the span ring buffer and sidecar attributes.
 */
static void
_csilk_otlp_state_free(csilk_server_t* server)
{
    otlp_state_t* st = (otlp_state_t*)server->otlp_state;
    if (!st) {
        return;
    }
    if (st->spans) {
        free(st->spans);
    }
    for (size_t i = 0; i < CSILK_OTLP_SIDECAR_CAP; i++) {
        if (st->sidecar_table[i].in_use && st->sidecar_table[i].attributes) {
            csilk_json_free(st->sidecar_table[i].attributes);
        }
    }
    free(st);
    server->otlp_state = NULL;
}

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

    _otlp_state_ensure();

    const char* tp = csilk_get_header(c, "traceparent");
    char        trace_id_buf[33] = {0};
    char        span_id_buf[17] = {0};
    char        parent_span_id[17] = {0};

    if (tp && strlen(tp) >= 55 && tp[0] == '0' && tp[1] == '0' && tp[2] == '-') {
        /* Reject non-hex trace/span ids: the 32/16 byte ids are later emitted
         * verbatim into response headers (X-Trace-Id, traceparent) and into the
         * APM spans JSON, so a hostile traceparent carrying CRLF or other
         * control bytes would inject headers / forge JSON. Fall back to random
         * ids unless every byte is a hex digit. */
        int hex_ok = 1;
        for (int i = 0; i < 32; i++) {
            hex_ok = hex_ok && isxdigit((unsigned char)tp[3 + i]);
        }
        for (int i = 0; i < 16 && hex_ok; i++) {
            hex_ok = hex_ok && isxdigit((unsigned char)tp[36 + i]);
        }
        if (hex_ok) {
            memcpy(trace_id_buf, tp + 3, 32);
            memcpy(parent_span_id, tp + 36, 16);
        } else {
            _csilk_generate_hex_id(trace_id_buf, 16);
        }
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
    }
    /* No arena (e.g. a bare context) means there is no request-scoped
     * storage to persist the ids into, so nothing is stored. The id strings
     * above are stack-local and must never escape the frame, so we do not
     * register them in the context on this path. */

    /* Push request trace context so child spans automatically inherit trace ID */
    csilk_otlp_push_trace_context(trace_id_buf, parent_span_id[0] ? parent_span_id : NULL);
    const char*        req_path = csilk_get_path(c);
    csilk_otlp_span_t* root_span = csilk_otlp_tracer_start_span(
        req_path ? req_path : "HTTP Request", parent_span_id[0] ? parent_span_id : NULL);
    csilk_set(c, "otlp_span", root_span);

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
    _otlp_state_ensure();

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
    csilk_otlp_span_t* s = NULL;
    if (c) {
        const char* tid = csilk_ctx_get_trace_id(c);
        const char* sid = csilk_ctx_get_span_id(c);
        if (tid && tid[0] != '\0') {
            csilk_otlp_push_trace_context(tid, sid);
            s = csilk_otlp_tracer_start_span(name, sid);
            if (tls_trace_depth >= 2) {
                tls_trace_stack[tls_trace_depth - 2] = tls_trace_stack[tls_trace_depth - 1];
                tls_trace_depth--;
            }
        } else {
            s = csilk_otlp_tracer_start_span(name, NULL);
        }
        csilk_set(c, "otlp_span", s);
        return s;
    }
    return csilk_otlp_tracer_start_span(name, NULL);
}

/**
 * @brief Finalize a span and append it to the global trace buffer.
 *
 * Records the end timestamp and status code, copies the span into the
 * ring-buffer of completed spans (g_otlp_state->spans) under the buffer mutex, then
 * frees the caller's span object.
 *
 * @param[in,out] span        The span returned by
 *                            csilk_otlp_tracer_start_span(). If NULL, returns
 *                            immediately.
 * @param[in]     status_code Final span status code.
 */
static void
_otlp_record_span_state(otlp_state_t* st, csilk_otlp_span_t* span)
{
    uint64_t total = atomic_fetch_add_explicit(&st->total_spans, 1, memory_order_relaxed) + 1;

    /* If buffer is at capacity, apply sampling: record only every sample_rate-th span */
    csilk_mutex_lock(&st->buffer_mutex);
    if (st->spans && st->count < st->cap) {
        st->spans[st->head] = *span;
        st->head = (st->head + 1) % st->cap;
        st->count++;
    } else if (st->spans && st->cap > 0) {
        /* Sampling path: only record if (total % sample_rate == 0) to keep stats representative */
        int sr = atomic_load_explicit(&st->sample_rate, memory_order_relaxed);
        if (sr > 0 && (total % (uint64_t)sr) == 0) {
            /* Overwrite the oldest span (ring buffer) */
            size_t idx = st->head;
            st->spans[idx] = *span;
            st->head = (idx + 1) % st->cap;
        }
    }
    csilk_mutex_unlock(&st->buffer_mutex);

    free(span);
}

/**
 * @brief Finalize a span and record it into a specific tracer state.
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
    otlp_state_t* st = _otlp_state_ensure();
    if (!st) {
        free(span);
        return;
    }
    _otlp_record_span_state(st, span);
}

/**
 * @brief Hook implementation for the scoped end_span callback.
 *
 * Resolves the tracer state from @p scope (the owning csilk_server_t*) when
 * provided, otherwise falls back to the process-level state. This is what
 * makes spans land in the owning server's ring buffer.
 */
static void
_otlp_hook_end_span(void* span, int status_code, void* scope)
{
    if (!span) {
        return;
    }
    csilk_otlp_span_t* s = (csilk_otlp_span_t*)span;
    s->end_time_ns = get_current_time_ns();
    s->status_code = status_code;
    if (tls_trace_depth > 0 &&
        strcmp(tls_trace_stack[tls_trace_depth - 1].span_id, s->span_id) == 0) {
        tls_trace_depth--;
    }
    otlp_state_t* st = NULL;
    if (scope) {
        csilk_server_t* server = (csilk_server_t*)scope;
        st = _otlp_state_ensure_server(server);
    }
    if (!st) {
        st = _otlp_state_ensure();
    }
    if (!st) {
        free(s);
        return;
    }
    _otlp_record_span_state(st, s);
}

/**
 * @brief Hook implementation for the scoped start_span callback.
 *
 * Span creation is buffer-independent (uses the TLS stack), so scope only
 * affects where the completed span is later recorded; the created span is
 * tagged with its owning scope so end_span can route it.
 */
static void*
_otlp_hook_start_span(const char* name, const char* parent_span_id, void* scope)
{
    (void)scope; /* start_span is buffer-independent; routing happens at end_span */
    return csilk_otlp_tracer_start_span(name, parent_span_id);
}

void
csilk_otlp_apm_spans_handler(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    otlp_state_t* st = _otlp_state_for_ctx(c);
    if (!st) {
        return;
    }
    csilk_mutex_lock(&st->buffer_mutex);
    csilk_json_t* arr = csilk_json_array();
    size_t        count = st->count;
    size_t        cap = st->cap;
    size_t        start = (st->head + cap - count) % cap;
    for (size_t i = 0; i < count; i++) {
        size_t                   idx = (start + i) % cap;
        const csilk_otlp_span_t* s = &st->spans[idx];
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

        csilk_mutex_lock(&st->sidecar_mutex);
        for (size_t k = 0; k < CSILK_OTLP_SIDECAR_CAP; k++) {
            if (st->sidecar_table[k].in_use &&
                strncmp(st->sidecar_table[k].span_id, s->span_id, 16) == 0) {
                if (st->sidecar_table[k].attributes) {
                    csilk_json_add_object(
                        obj, "attributes", csilk_json_copy(st->sidecar_table[k].attributes));
                }
                break;
            }
        }
        csilk_mutex_unlock(&st->sidecar_mutex);

        csilk_json_array_append(arr, obj);
    }
    csilk_mutex_unlock(&st->buffer_mutex);

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
 * @brief Set the sampling rate applied when the span ring buffer is full.
 *
 * When the buffer is at capacity, only every N-th completed span is recorded
 * (overwriting the oldest entry). A value of 0 disables sampling (overwrite
 * unconditionally). Default is 1000 (sample 1/1000).
 *
 * @param rate  Sampling interval (0 = always overwrite, N = 1/N sampling).
 */
void
csilk_otlp_tracer_set_sample_rate(int rate)
{
    otlp_state_t* st = _otlp_state_ensure();
    if (!st) {
        return;
    }
    atomic_store_explicit(&st->sample_rate, rate, memory_order_relaxed);
    CSILK_LOG_D("OTLP: sample rate set to %d", rate);
}

void
csilk_otlp_tracer_set_buffer_size(size_t cap)
{
    if (cap > 0 && !atomic_load_explicit(&g_otlp_state->total_spans, memory_order_acquire)) {
        otlp_state_t* st = g_otlp_state;
        if (st && !st->spans) {
            st->cap = cap;
        }
        CSILK_LOG_D("OTLP: span buffer capacity set to %zu", cap);
    }
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

int
csilk_otlp_span_bind_ai_trace(csilk_otlp_span_t* span, const struct csilk_ai_trace_s* trace)
{
    if (!span || !trace) {
        return -1;
    }
    otlp_state_t* st = _otlp_state_ensure();
    if (!st) {
        return -1;
    }
    csilk_mutex_lock(&st->sidecar_mutex);

    csilk_otlp_sidecar_entry_t* target = NULL;
    for (size_t i = 0; i < CSILK_OTLP_SIDECAR_CAP; i++) {
        if (st->sidecar_table[i].in_use &&
            strncmp(st->sidecar_table[i].span_id, span->span_id, 16) == 0) {
            target = &st->sidecar_table[i];
            break;
        }
    }
    if (!target) {
        for (size_t i = 0; i < CSILK_OTLP_SIDECAR_CAP; i++) {
            if (!st->sidecar_table[i].in_use) {
                target = &st->sidecar_table[i];
                strncpy(target->span_id, span->span_id, sizeof(target->span_id) - 1);
                target->span_id[sizeof(target->span_id) - 1] = '\0';
                target->attributes = csilk_json_object();
                target->in_use = true;
                break;
            }
        }
    }
    if (!target) {
        target = &st->sidecar_table[0];
        if (target->attributes) {
            csilk_json_free(target->attributes);
        }
        strncpy(target->span_id, span->span_id, sizeof(target->span_id) - 1);
        target->span_id[sizeof(target->span_id) - 1] = '\0';
        target->attributes = csilk_json_object();
        target->in_use = true;
    }

    /* Set OpenTelemetry semantic conventions for GenAI */
    csilk_json_add_int(
        target->attributes, "gen_ai.usage.prompt_tokens", (int64_t)trace->prompt_tokens);
    csilk_json_add_int(
        target->attributes, "gen_ai.usage.completion_tokens", (int64_t)trace->completion_tokens);
    csilk_json_add_int(
        target->attributes, "gen_ai.usage.total_tokens", (int64_t)trace->total_tokens);
    csilk_json_add_number(
        target->attributes, "gen_ai.client.ttft_ms", (double)trace->first_token_ms);
    csilk_json_add_number(
        target->attributes, "gen_ai.client.duration_ms", (double)trace->latency_ms);
    csilk_json_add_number(target->attributes, "gen_ai.usage.cost_usd", trace->cost_usd);

    if (trace->model[0] != '\0') {
        csilk_json_add_string(target->attributes, "gen_ai.response.model", trace->model);
    }
    if (trace->provider[0] != '\0') {
        csilk_json_add_string(target->attributes, "gen_ai.system", trace->provider);
    }

    csilk_mutex_unlock(&st->sidecar_mutex);

    /* If tool_spans JSON is present and non-empty, create child spans for tool calls */
    if (trace->tool_spans[0] == '[' && trace->tool_spans[1] != ']') {
        csilk_json_t* tool_arr = csilk_json_parse(trace->tool_spans);
        if (tool_arr && csilk_json_is_array(tool_arr)) {
            size_t   num_tools = csilk_json_array_size(tool_arr);
            uint64_t child_start_ns = span->start_time_ns;
            for (size_t t = 0; t < num_tools; t++) {
                const csilk_json_t* item = csilk_json_array_get(tool_arr, t);
                if (item) {
                    const char* name = csilk_json_get_string(item, "name");
                    double      lat_ms = csilk_json_get_number(item, "latency_ms");
                    if (lat_ms <= 0.0) {
                        lat_ms = 1.0;
                    }
                    char tool_span_name[128];
                    snprintf(
                        tool_span_name, sizeof(tool_span_name), "ai.tool.%s", name ? name : "call");
                    csilk_otlp_span_t* child_span =
                        csilk_otlp_tracer_start_span(tool_span_name, span->span_id);
                    if (child_span) {
                        child_span->start_time_ns = child_start_ns;
                        child_span->end_time_ns = child_start_ns + (uint64_t)(lat_ms * 1000000.0);
                        child_start_ns = child_span->end_time_ns;
                        csilk_otlp_tracer_end_span(child_span, 1);
                    }
                }
            }
            csilk_json_free(tool_arr);
        }
    }

    return 0;
}

int
csilk_otlp_ctx_attach_ai_trace(csilk_ctx_t* ctx, const struct csilk_ai_trace_s* trace)
{
    if (!ctx || !trace) {
        return -1;
    }
    csilk_set(ctx, "ai_trace", (void*)trace);
    csilk_otlp_span_t* span = (csilk_otlp_span_t*)csilk_get(ctx, "otlp_span");
    if (!span) {
        return 0;
    }
    return csilk_otlp_span_bind_ai_trace(span, trace);
}

const struct csilk_ai_trace_s*
csilk_otlp_ctx_get_ai_trace(csilk_ctx_t* ctx)
{
    if (!ctx) {
        return NULL;
    }
    return (const struct csilk_ai_trace_s*)csilk_get(ctx, "ai_trace");
}

int
csilk_otlp_export_span_json(const csilk_otlp_span_t* span,
                            char*                    out_buf,
                            size_t                   cap,
                            size_t*                  out_len)
{
    if (!span || !out_buf || cap == 0) {
        return -1;
    }
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "traceId", span->trace_id);
    csilk_json_add_string(obj, "spanId", span->span_id);
    if (span->parent_span_id[0] != '\0') {
        csilk_json_add_string(obj, "parentSpanId", span->parent_span_id);
    }
    csilk_json_add_string(obj, "name", span->name);
    csilk_json_add_number(obj, "startTimeUnixNano", (double)span->start_time_ns);
    csilk_json_add_number(obj, "endTimeUnixNano", (double)span->end_time_ns);
    double dur_ms = (span->end_time_ns >= span->start_time_ns)
                        ? (double)(span->end_time_ns - span->start_time_ns) / 1000000.0
                        : 0.0;
    csilk_json_add_number(obj, "durationMs", dur_ms);
    csilk_json_add_number(obj, "statusCode", span->status_code);

    /* Lookup sidecar attributes if present */
    otlp_state_t* st = _otlp_state_ensure();
    if (!st) {
        return -1;
    }
    csilk_mutex_lock(&st->sidecar_mutex);
    for (size_t i = 0; i < CSILK_OTLP_SIDECAR_CAP; i++) {
        if (st->sidecar_table[i].in_use &&
            strncmp(st->sidecar_table[i].span_id, span->span_id, 16) == 0) {
            if (st->sidecar_table[i].attributes) {
                csilk_json_add_object(
                    obj, "attributes", csilk_json_copy(st->sidecar_table[i].attributes));
            }
            break;
        }
    }
    csilk_mutex_unlock(&st->sidecar_mutex);

    size_t sz = 0;
    char*  str = csilk_json_serialize(obj, &sz);
    csilk_json_free(obj);
    if (!str) {
        return -1;
    }
    if (sz >= cap) {
        free(str);
        return -1;
    }
    memcpy(out_buf, str, sz);
    out_buf[sz] = '\0';
    if (out_len) {
        *out_len = sz;
    }
    free(str);
    return 0;
}
