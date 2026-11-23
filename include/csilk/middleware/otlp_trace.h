/**
 * @file otlp_trace.h
 * @brief OpenTelemetry distributed tracing and APM Dashboard for csilk.
 * @copyright MIT License
 */

#ifndef CSILK_OTLP_TRACE_H
#define CSILK_OTLP_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/app/app.h"
#include "csilk/core/middleware.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts a new OpenTelemetry Trace Span.
 * @param name Operation name.
 * @param parent_span_id Optional parent span ID string (NULL if root span).
 * @return Allocated span handle.
 */
csilk_otlp_span_t* csilk_otlp_tracer_start_span(const char* name, const char* parent_span_id);

/**
 * @brief Starts a child span associated with an explicit request context.
 * @param c The request context.
 * @param name Operation name.
 * @return Allocated child span handle.
 */
csilk_otlp_span_t* csilk_otlp_tracer_start_span_ctx(csilk_ctx_t* c, const char* name);

/**
 * @brief Push a trace/span context to the current thread-local trace stack.
 * @param trace_id Trace ID string (or NULL).
 * @param span_id Span ID string (or NULL).
 */
void csilk_otlp_push_trace_context(const char* trace_id, const char* span_id);

/**
 * @brief Pop the top trace/span context from the current thread-local trace stack.
 */
void csilk_otlp_pop_trace_context(void);

/**
 * @brief Retrieve the current active trace context for the calling thread.
 * @param out_trace_id Destination buffer for trace ID (min 33 bytes, or NULL).
 * @param tid_sz Capacity of out_trace_id.
 * @param out_span_id Destination buffer for span ID (min 17 bytes, or NULL).
 * @param sid_sz Capacity of out_span_id.
 * @return 0 on success, -1 if no active trace context.
 */
int csilk_otlp_get_current_trace_context(char*  out_trace_id,
                                         size_t tid_sz,
                                         char*  out_span_id,
                                         size_t sid_sz);

/**
 * @brief Ends the span, records nanosecond duration, and pushes to ring buffer.
 * @param span Span handle.
 * @param status_code Status (0: UNSET, 1: OK, 2: ERROR).
 */
void csilk_otlp_tracer_end_span(csilk_otlp_span_t* span, int status_code);

/**
 * @brief HTTP handler that serves the embedded APM UI HTML page.
 * @param c The request context.
 */
void csilk_otlp_apm_ui_handler(csilk_ctx_t* c);

/**
 * @brief HTTP handler that returns JSON array of buffered telemetry spans.
 * @param c The request context.
 */
void csilk_otlp_apm_spans_handler(csilk_ctx_t* c);

/**
 * @brief Registers embedded APM Web Dashboard UI routes.
 * @param app Application handle.
 * @param path Base URL path (e.g., "/admin/apm").
 */
void csilk_otlp_serve_apm_ui(csilk_app_t* app, const char* path);

/**
 * @brief Set the sampling rate applied when the span ring buffer is full.
 * @param rate Sampling interval (0 = always overwrite, N = 1/N sampling). Default 1000.
 */
void csilk_otlp_tracer_set_sample_rate(int rate);

/**
 * @brief Set the span ring buffer capacity (must be called before first span record).
 * @param cap New capacity (must be > 0). Default 2048.
 */
void csilk_otlp_tracer_set_buffer_size(size_t cap);

struct csilk_ai_trace_s;

/**
 * @brief Bind AI inference trace metrics and tool spans to an OTLP Span.
 *
 * Populates GenAI attributes (gen_ai.usage.prompt_tokens, completion_tokens,
 * response.model, etc.) in the sidecar attribute table and creates tool child spans.
 *
 * @param span Target OTLP Span handle.
 * @param trace AI trace container holding tokens, latency, and tool spans.
 * @return 0 on success, -1 on invalid arguments.
 */
int csilk_otlp_span_bind_ai_trace(csilk_otlp_span_t* span, const struct csilk_ai_trace_s* trace);

/**
 * @brief Automatically attach AI trace to the active request context's OTLP span.
 *
 * @param ctx HTTP request context.
 * @param trace AI trace container.
 * @return 0 on success, -1 if no active span or invalid arguments.
 */
int csilk_otlp_ctx_attach_ai_trace(csilk_ctx_t* ctx, const struct csilk_ai_trace_s* trace);

/**
 * @brief Export a single OTLP span and its sidecar attributes to JSON format.
 *
 * @param span Span handle.
 * @param out_buf Destination JSON buffer.
 * @param cap Capacity of out_buf.
 * @param out_len Destination pointer to receive the written string length.
 * @return 0 on success, -1 on buffer overflow or serialization error.
 */
int csilk_otlp_export_span_json(const csilk_otlp_span_t* span,
                                char*                    out_buf,
                                size_t                   cap,
                                size_t*                  out_len);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_OTLP_TRACE_H */
