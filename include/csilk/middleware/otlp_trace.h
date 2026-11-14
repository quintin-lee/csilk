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

#ifdef __cplusplus
}
#endif

#endif /* CSILK_OTLP_TRACE_H */
