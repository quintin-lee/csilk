/**
 * @file otlp_exporter.c
 * @brief OpenTelemetry (OTLP) Tracing Span exporter implementation.
 *
 * Provides batching, thread-safe queuing, and formatting of OpenTelemetry spans
 * into standard OTLP/JSON format (`resourceSpans` / `scopeSpans` schema) for consumption
 * by OpenTelemetry Collectors, Jaeger, or Zipkin.
 */

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Internal OTLP Exporter state structure.
 */
struct csilk_otlp_exporter_s {
    char               endpoint_url[256]; /**< Exporter HTTP/gRPC endpoint URL */
    int                batch_size;        /**< Maximum spans buffer capacity */
    csilk_otlp_span_t* spans;             /**< Array of buffered spans */
    int                count;             /**< Current buffered span count */
    csilk_mutex_t      mutex;             /**< Mutex protecting span buffer */
};

/**
 * @brief Create a new OpenTelemetry Span Exporter instance.
 *
 * @param endpoint_url OTLP Collector endpoint URL (e.g. "http://localhost:4318/v1/traces").
 * @param batch_size Maximum buffer size for span batching before flushing.
 * @return Exporter handle, or NULL on memory allocation failure.
 */
csilk_otlp_exporter_t*
csilk_otlp_exporter_new(const char* endpoint_url, int batch_size)
{
    csilk_otlp_exporter_t* exp = malloc(sizeof(csilk_otlp_exporter_t));
    if (!exp) {
        return NULL;
    }
    snprintf(exp->endpoint_url,
             sizeof(exp->endpoint_url),
             "%s",
             endpoint_url ? endpoint_url : "http://localhost:4318/v1/traces");
    exp->batch_size = batch_size > 0 ? batch_size : 100;
    exp->spans = calloc((size_t)exp->batch_size, sizeof(csilk_otlp_span_t));
    exp->count = 0;
    csilk_mutex_init(&exp->mutex);
    return exp;
}

/**
 * @brief Record a finished span into the exporter's queue.
 *
 * @param exp Exporter instance.
 * @param span Pointer to completed span record to copy into buffer.
 */
void
csilk_otlp_exporter_record_span(csilk_otlp_exporter_t* exp, const csilk_otlp_span_t* span)
{
    if (!exp || !span) {
        return;
    }
    csilk_mutex_lock(&exp->mutex);
    if (exp->count < exp->batch_size) {
        exp->spans[exp->count] = *span;
        exp->count++;
    }
    csilk_mutex_unlock(&exp->mutex);
}

/**
 * @brief Serialize buffered spans into OpenTelemetry OTLP/JSON payload (`resourceSpans`).
 *
 * Flushes the current queue upon successful formatting.
 *
 * @param exp Exporter instance.
 * @param[out] out_buf Destination character buffer.
 * @param buf_size Capacity of @p out_buf.
 * @return Number of exported spans, or -1 on error.
 */
int
csilk_otlp_exporter_export_json(csilk_otlp_exporter_t* exp, char* out_buf, size_t buf_size)
{
    if (!exp || !out_buf || buf_size == 0) {
        return -1;
    }
    csilk_mutex_lock(&exp->mutex);
    int exported_count = exp->count;

    /* Build OTLP JSON hierarchy: root -> resourceSpans -> scopeSpans -> spans */
    csilk_json_t* root = csilk_json_object();
    csilk_json_t* resource_spans = csilk_json_array();
    csilk_json_add_object(root, "resourceSpans", resource_spans);

    csilk_json_t* res_span = csilk_json_object();
    csilk_json_array_append(resource_spans, res_span);

    csilk_json_t* scope_spans = csilk_json_array();
    csilk_json_add_object(res_span, "scopeSpans", scope_spans);

    csilk_json_t* scope_span = csilk_json_object();
    csilk_json_array_append(scope_spans, scope_span);

    csilk_json_t* spans_arr = csilk_json_array();
    csilk_json_add_object(scope_span, "spans", spans_arr);

    for (int i = 0; i < exp->count; i++) {
        csilk_json_t* s = csilk_json_object();
        csilk_json_add_string(s, "traceId", exp->spans[i].trace_id);
        csilk_json_add_string(s, "spanId", exp->spans[i].span_id);
        if (exp->spans[i].parent_span_id[0] != '\0') {
            csilk_json_add_string(s, "parentSpanId", exp->spans[i].parent_span_id);
        }
        csilk_json_add_string(s, "name", exp->spans[i].name);
        csilk_json_add_number(s, "startTimeUnixNano", (double)exp->spans[i].start_time_ns);
        csilk_json_add_number(s, "endTimeUnixNano", (double)exp->spans[i].end_time_ns);
        csilk_json_add_number(s, "statusCode", exp->spans[i].status_code);
        csilk_json_array_append(spans_arr, s);
    }

    char* json_str = csilk_json_serialize(root, NULL);
    if (json_str) {
        snprintf(out_buf, buf_size, "%s", json_str);
        free(json_str);
    }
    csilk_json_free(root);

    /* Reset buffer count after flush */
    exp->count = 0;
    csilk_mutex_unlock(&exp->mutex);
    return exported_count;
}

int
csilk_otlp_exporter_export_grpc(csilk_otlp_exporter_t* exp, uint8_t* out_buf, size_t buf_size)
{
    if (!exp || !out_buf || buf_size < 16) {
        return -1;
    }

    char json_buf[4096] = {0};
    int  count = csilk_otlp_exporter_export_json(exp, json_buf, sizeof(json_buf));
    if (count <= 0) {
        return -1;
    }

    size_t json_len = strlen(json_buf);
    int frame_len = csilk_grpc_frame_encode((const uint8_t*)json_buf, json_len, out_buf, buf_size);
    return frame_len;
}

/**
 * @brief Free resources associated with an OTLP Exporter.
 *
 * @param exp Exporter instance.
 */
void
csilk_otlp_exporter_free(csilk_otlp_exporter_t* exp)
{
    if (!exp) {
        return;
    }
    csilk_mutex_destroy(&exp->mutex);
    if (exp->spans) {
        free(exp->spans);
    }
    free(exp);
}
