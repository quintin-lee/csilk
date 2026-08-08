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
 * @brief Ends the span, records nanosecond duration, and pushes to ring buffer.
 * @param span Span handle.
 * @param status_code Status (0: UNSET, 1: OK, 2: ERROR).
 */
void csilk_otlp_tracer_end_span(csilk_otlp_span_t* span, int status_code);

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
