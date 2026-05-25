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

/** @brief Global counter for total HTTP requests. */
static _Atomic uint64_t http_requests_total = 0;

/** @brief Global counter for total HTTP request duration in microseconds. */
static _Atomic uint64_t http_request_duration_microseconds = 0;

/** @brief Prometheus metrics middleware.
 *  Measures request duration and increments global counters.
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

/** @brief Handler for /metrics endpoint.
 *  Returns metrics in Prometheus text format.
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
