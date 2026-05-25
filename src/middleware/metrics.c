/**
 * @file metrics.c
 * @brief Prometheus metrics middleware and handler.
 * @copyright MIT License
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "csilk.h"

/**
 * @brief Global atomic counter for total HTTP requests processed.
 *
 * Incremented once per request by csilk_metrics_middleware. Accessed
 * concurrently by all worker threads — must use atomic operations.
 */
static _Atomic uint64_t http_requests_total = 0;

/**
 * @brief Global atomic counter for cumulative HTTP request duration.
 *
 * Stores the sum of all request durations in microseconds. Combined with
 * http_requests_total, this allows computing average latency at the
 * /metrics endpoint. Accessed via atomic_load for thread safety.
 */
static _Atomic uint64_t http_request_duration_microseconds = 0;

/**
 * @brief Prometheus metrics middleware.
 *
 * Measures the duration of the entire request handling chain using a high-
 * resolution timer (uv_hrtime) started before csilk_next() and stopped after
 * all downstream handlers have completed. Increments global atomic counters
 * for total request count and cumulative latency in microseconds.
 *
 * @param c   The request context.
 * @param arg Unused configuration parameter (reserved).
 *
 * @note The arg parameter is currently unused and should be passed as NULL.
 * @warning Uses atomic operations for thread safety — counters may have
 *          minor skew under extreme contention but remain accurate enough
 *          for monitoring purposes.
 */
void csilk_metrics_middleware(csilk_ctx_t* c, const char* arg) {
  (void)arg;
  uint64_t start = uv_hrtime();

  csilk_next(c);

  uint64_t duration_ns = uv_hrtime() - start;
  uint64_t duration_us = duration_ns / 1000;

  atomic_fetch_add(&http_requests_total, 1);
  atomic_fetch_add(&http_request_duration_microseconds, duration_us);
}

/**
 * @brief Handler for the /metrics endpoint.
 *
 * Collects the global atomic counters and renders a response in the standard
 * Prometheus text-based exposition format (content type
 * text/plain; version=0.0.4). Includes HELP and TYPE metadata lines for both
 * http_requests_total and http_request_duration_microseconds counters.
 *
 * @param c  The request context.
 *
 * @note The output buffer is fixed at 512 bytes, sufficient for the two
 *       counter metrics. Extending this handler to support additional or
 *       dynamic metrics will require a larger or dynamically-sized buffer.
 */
void csilk_metrics_handler(csilk_ctx_t* c) {
  uint64_t total_requests = atomic_load(&http_requests_total);
  uint64_t total_duration = atomic_load(&http_request_duration_microseconds);

  char buf[512];
  snprintf(buf, sizeof(buf),
           "# HELP http_requests_total Total number of HTTP requests\n"
           "# TYPE http_requests_total counter\n"
           "http_requests_total %llu\n"
           "# HELP http_request_duration_microseconds Total duration of HTTP "
           "requests in microseconds\n"
           "# TYPE http_request_duration_microseconds counter\n"
           "http_request_duration_microseconds %llu\n",
           (unsigned long long)total_requests,
           (unsigned long long)total_duration);

  csilk_set_header(c, "Content-Type", "text/plain; version=0.0.4");
  csilk_string(c, CSILK_STATUS_OK, buf);
}
