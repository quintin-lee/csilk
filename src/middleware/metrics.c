/**
 * @file metrics.c
 * @brief Advanced Prometheus metrics middleware with dimensional labels and
 * histograms.
 *
 * This module implements a high-performance metrics collection system for the
 * csilk framework. It supports:
 * - Aggregated global metrics (requests, duration).
 * - Dimensional metrics with labels (method, route, status).
 * - Latency histograms for P99/P95 analysis.
 * - Lock-free atomic updates using a fixed-size hash table.
 * - System-wide telemetry (CPU, Memory, MQ, AI, DB, Security).
 *
 * @copyright MIT License
 */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"

/**
 * @brief Latency buckets in seconds for P99/P95 analysis.
 *
 * These values define the upper bounds (le="...") for Prometheus histogram
 * buckets.
 */
static const double CSILK_METRICS_BUCKETS[CSILK_METRICS_BUCKET_COUNT] = {
    0.01, 0.05, 0.1, 0.5, 1.0, 5.0};

/* --- Global Metrics (Aggregated) --- */

/** @brief Atomic counter for total HTTP requests across all workers. */
static _Atomic uint64_t http_requests_total = 0;

/** @brief Atomic counter for cumulative request duration in microseconds. */
static _Atomic uint64_t http_request_duration_microseconds = 0;

/* --- Security-specific Atomic Counters --- */
static _Atomic uint64_t security_rate_limit_blocks = 0;
static _Atomic uint64_t security_csrf_violations = 0;
static _Atomic uint64_t security_auth_failures = 0;

/* --- Dimensional Metrics Storage --- */

/**
 * @brief Data structure for a specific combination of labels.
 *
 * Tracks requests that match a specific HTTP method, status code, and
 * route pattern (e.g., GET 200 /api/users/:id).
 */
typedef struct {
	char method[12];	      /**< HTTP method (GET, POST, etc.) */
	char route[128];	      /**< Matched route pattern (e.g., "/users/:id") */
	int status;		      /**< HTTP response status code */
	_Atomic uint64_t count;	      /**< Total requests for this dimension */
	_Atomic uint64_t duration_us; /**< Total duration in microseconds */
	_Atomic uint64_t buckets[CSILK_METRICS_BUCKET_COUNT + 1]; /**< Histogram bucket counts */
	_Atomic int in_use; /**< Lock-free flag for slot occupation */
} csilk_route_metric_t;

/**
 * @brief Fixed-size hash table for route metrics using open addressing.
 *
 * Statically allocated to ensure zero heap allocation during request
 * processing. Initialized to zero (all slots available).
 */
static csilk_route_metric_t route_metrics[CSILK_METRICS_MAX_ENTRIES] = {0};

/* --- Internal Helper Functions --- */

/**
 * @brief Computes a hash for the metric dimension triple.
 *
 * Uses the djb2 algorithm for stable distribution across the hash table.
 *
 * @param method The HTTP method string.
 * @param route  The route pattern string.
 * @param status The HTTP status code.
 * @return A hash index within [0, CSILK_METRICS_MAX_ENTRIES).
 */
static uint32_t
metrics_hash(const char* method, const char* route, int status)
{
	uint32_t hash = 5381;
	int c;
	while ((c = *method++)) {
		hash = ((hash << 5) + hash) + c;
	}
	while ((c = *route++)) {
		hash = ((hash << 5) + hash) + c;
	}
	hash = ((hash << 5) + hash) + status;
	return hash % CSILK_METRICS_MAX_ENTRIES;
}

/**
 * @brief Finds or allocates a thread-safe slot in the metrics hash table.
 *
 * Implements a lock-free "claim" mechanism using atomic_compare_exchange.
 *
 * @param method The HTTP method.
 * @param route  The route pattern.
 * @param status The status code.
 * @return Pointer to the metric slot, or nullptr if the table is saturated.
 */
static csilk_route_metric_t*
get_metric_slot(const char* method, const char* route, int status)
{
	uint32_t start_idx = metrics_hash(method, route, status);

	/* Linear probing for open addressing */
	for (uint32_t i = 0; i < CSILK_METRICS_MAX_ENTRIES; i++) {
		uint32_t idx = (start_idx + i) % CSILK_METRICS_MAX_ENTRIES;
		csilk_route_metric_t* slot = &route_metrics[idx];

		/* Attempt to claim the slot if it's currently empty (in_use == 0) */
		int expected = 0;
		if (atomic_compare_exchange_strong(&slot->in_use, &expected, 1)) {
			/* Success: We claimed a new slot. Initialize its keys. */
			strncpy(slot->method, method, sizeof(slot->method) - 1);
			strncpy(slot->route, route, sizeof(slot->route) - 1);
			slot->status = status;
			return slot;
		}

		/* Slot is already occupied. Check if it matches our keys. */
		if (slot->status == status && strcmp(slot->method, method) == 0 &&
		    strcmp(slot->route, route) == 0) {
			return slot;
		}
	}
	return nullptr; /* Table is full; telemetry for this combo will be dropped. */
}

/* --- Public API: Statistics Collection --- */

void
csilk_security_get_stats(csilk_security_stats_t* stats)
{
	if (!stats) {
		return;
	}
	stats->rate_limit_blocks = atomic_load(&security_rate_limit_blocks);
	stats->csrf_violations = atomic_load(&security_csrf_violations);
	stats->auth_failures = atomic_load(&security_auth_failures);
}

void
csilk_process_get_stats(csilk_process_stats_t* stats)
{
	if (!stats) {
		return;
	}

	/* Get resident memory (RSS) from libuv */
	uv_resident_set_memory(&stats->rss_bytes);

	/* Get CPU time (User/System) from libuv */
	uv_rusage_t usage;
	if (uv_getrusage(&usage) == 0) {
		stats->cpu_user_time_sec =
		    (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1000000.0;
		stats->cpu_sys_time_sec =
		    (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1000000.0;
	}
}

void
_csilk_metrics_inc_rate_limit_blocks(void)
{
	atomic_fetch_add(&security_rate_limit_blocks, 1);
}

void
_csilk_metrics_inc_csrf_violations(void)
{
	atomic_fetch_add(&security_csrf_violations, 1);
}

void
_csilk_metrics_inc_auth_failures(void)
{
	atomic_fetch_add(&security_auth_failures, 1);
}

/* --- Middleware Implementation --- */

/**
 * @brief Dimensional metrics middleware.
 *
 * Wraps the request execution to measure latency and update atomic counters.
 */
void
csilk_metrics_middleware(csilk_ctx_t* c, const char* arg)
{
	(void)arg;

	/* Start high-resolution timer */
	uint64_t start = uv_hrtime();

	/* Proceed with next middleware/handler */
	csilk_next(c);

	/* Calculate duration */
	uint64_t duration_ns = uv_hrtime() - start;
	double duration_sec = (double)duration_ns / 1e9;
	uint64_t duration_us = duration_ns / 1000;

	/* 1. Update Global Aggregate Metrics (Fast path) */
	atomic_fetch_add(&http_requests_total, 1);
	atomic_fetch_add(&http_request_duration_microseconds, duration_us);

	/* 2. Update Dimensional Route Metrics & Histograms */
	const char* method = csilk_get_method(c) ? csilk_get_method(c) : "UNKNOWN";

	/* Use the static route pattern (e.g., /users/:id) if available, otherwise
   * fall back. */
	const char* route = csilk_ctx_get_handler_path(c);
	if (!route) {
		route = "unmatched";
	}
	int status = csilk_get_status(c);

	csilk_route_metric_t* slot = get_metric_slot(method, route, status);
	if (slot) {
		/* Atomically update count and duration for this dimension */
		atomic_fetch_add(&slot->count, 1);
		atomic_fetch_add(&slot->duration_us, duration_us);

		/* Update Histogram Buckets (cumulative) */
		for (int i = 0; i < CSILK_METRICS_BUCKET_COUNT; i++) {
			if (duration_sec <= CSILK_METRICS_BUCKETS[i]) {
				atomic_fetch_add(&slot->buckets[i], 1);
			}
		}
		/* The +Inf bucket always increments */
		atomic_fetch_add(&slot->buckets[CSILK_METRICS_BUCKET_COUNT], 1);
	}
}

/* --- Prometheus Exposition Handler --- */

/**
 * @brief Dynamic string builder for Prometheus text format.
 *
 * Automatically reallocates the buffer as needed.
 */
static void
append_metric(char** buf, size_t* size, size_t* offset, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(nullptr, 0, fmt, args);
	va_end(args);

	if (n < 0) {
		return;
	}

	/* Ensure sufficient space (+1 for null terminator) */
	if (*offset + n + 1 > *size) {
		size_t new_size = (*size == 0) ? 4096 : *size * 2;
		while (*offset + n + 1 > new_size) {
			new_size *= 2;
		}
		char* new_buf = realloc(*buf, new_size);
		if (!new_buf) {
			return;
		}
		*buf = new_buf;
		*size = new_size;
	}

	va_start(args, fmt);
	vsnprintf(*buf + *offset, *size - *offset, fmt, args);
	va_end(args);
	*offset += n;
}

/**
 * @brief Handler for the /metrics endpoint.
 *
 * Renders full system telemetry in standard Prometheus text-based format.
 */
void
csilk_metrics_handler(csilk_ctx_t* c)
{
	char* buf = nullptr;
	size_t size = 0, offset = 0;

	/* --- Part 1: Aggregated HTTP Metrics (Compatibility) --- */
	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP http_requests_total_agg Total number of HTTP requests "
		      "(aggregated)\n");
	append_metric(&buf, &size, &offset, "# TYPE http_requests_total_agg counter\n");
	append_metric(&buf,
		      &size,
		      &offset,
		      "http_requests_total_agg %llu\n",
		      (unsigned long long)atomic_load(&http_requests_total));

	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP http_request_duration_microseconds_agg Total duration "
		      "in microseconds (aggregated)\n");
	append_metric(
	    &buf, &size, &offset, "# TYPE http_request_duration_microseconds_agg counter\n");
	append_metric(&buf,
		      &size,
		      &offset,
		      "http_request_duration_microseconds_agg %llu\n",
		      (unsigned long long)atomic_load(&http_request_duration_microseconds));

	/* --- Part 2: Dimensional HTTP Metrics (Labels + Histogram) --- */
	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP http_requests_total Total number of HTTP requests by "
		      "method, status, route\n");
	append_metric(&buf, &size, &offset, "# TYPE http_requests_total counter\n");
	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP http_request_duration_seconds Histogram of "
		      "request durations\n");
	append_metric(&buf, &size, &offset, "# TYPE http_request_duration_seconds histogram\n");

	for (int i = 0; i < CSILK_METRICS_MAX_ENTRIES; i++) {
		csilk_route_metric_t* slot = &route_metrics[i];
		if (atomic_load(&slot->in_use)) {
			uint64_t count = atomic_load(&slot->count);
			double dur_sec = (double)atomic_load(&slot->duration_us) / 1e6;

			/* Request Counts */
			append_metric(&buf,
				      &size,
				      &offset,
				      "http_requests_total{method=\"%s\","
				      "status=\"%d\",route=\"%"
				      "s\"} %llu\n",
				      slot->method,
				      slot->status,
				      slot->route,
				      (unsigned long long)count);

			/* Histogram Summary (Sum + Count) */
			append_metric(&buf,
				      &size,
				      &offset,
				      "http_request_duration_seconds_sum{"
				      "method=\"%s\",status=\"%"
				      "d\",route=\"%s\"} %.6f\n",
				      slot->method,
				      slot->status,
				      slot->route,
				      dur_sec);
			append_metric(&buf,
				      &size,
				      &offset,
				      "http_request_duration_seconds_count{"
				      "method=\"%s\",status="
				      "\"%d\",route=\"%s\"} %llu\n",
				      slot->method,
				      slot->status,
				      slot->route,
				      (unsigned long long)count);

			/* Histogram Buckets */
			for (int b = 0; b < CSILK_METRICS_BUCKET_COUNT; b++) {
				append_metric(&buf,
					      &size,
					      &offset,
					      "http_request_duration_seconds_"
					      "bucket{method=\"%s\","
					      "status=\"%d\",route=\"%s\",le="
					      "\"%.2f\"} %llu\n",
					      slot->method,
					      slot->status,
					      slot->route,
					      CSILK_METRICS_BUCKETS[b],
					      (unsigned long long)atomic_load(&slot->buckets[b]));
			}
			append_metric(&buf,
				      &size,
				      &offset,
				      "http_request_duration_seconds_bucket{method=\"%"
				      "s\",status="
				      "\"%d\",route=\"%s\",le=\"+Inf\"} %llu\n",
				      slot->method,
				      slot->status,
				      slot->route,
				      (unsigned long long)atomic_load(
					  &slot->buckets[CSILK_METRICS_BUCKET_COUNT]));
		}
	}

	/* --- Part 3: System & Process Telemetry --- */
	csilk_process_stats_t proc;
	csilk_process_get_stats(&proc);
	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP process_resident_memory_bytes Resident set size "
		      "in bytes\n");
	append_metric(&buf, &size, &offset, "process_resident_memory_bytes %zu\n", proc.rss_bytes);
	append_metric(&buf,
		      &size,
		      &offset,
		      "# HELP process_cpu_seconds_total Total user and system CPU "
		      "time in seconds\n");
	append_metric(&buf,
		      &size,
		      &offset,
		      "process_cpu_seconds_total{type=\"user\"} %.3f\n",
		      proc.cpu_user_time_sec);
	append_metric(&buf,
		      &size,
		      &offset,
		      "process_cpu_seconds_total{type=\"sys\"} %.3f\n",
		      proc.cpu_sys_time_sec);

	/* --- Part 4: Security & Framework Integrity --- */
	csilk_security_stats_t sec;
	csilk_security_get_stats(&sec);
	append_metric(&buf,
		      &size,
		      &offset,
		      "csilk_security_rate_limit_blocks_total %llu\n",
		      (unsigned long long)sec.rate_limit_blocks);
	append_metric(&buf,
		      &size,
		      &offset,
		      "csilk_security_csrf_violations_total %llu\n",
		      (unsigned long long)sec.csrf_violations);
	append_metric(&buf,
		      &size,
		      &offset,
		      "csilk_security_auth_failures_total %llu\n",
		      (unsigned long long)sec.auth_failures);

	/* --- Part 5: Server Internal State --- */
	int active = 0, pooled = 0;
	csilk_server_get_stats(csilk_ctx_get_server(c), &active, &pooled);
	append_metric(&buf, &size, &offset, "csilk_server_active_connections %d\n", active);
	append_metric(&buf, &size, &offset, "csilk_server_pooled_connections %d\n", pooled);

	/* --- Part 6: Sub-system Statistics (Optional) --- */

	/* MQ Stats */
	csilk_mq_stats_t mq;
	csilk_mq_get_stats(csilk_server_get_mq(csilk_ctx_get_server(c)), &mq);
	if (mq.published_total > 0) {
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_mq_messages_total{action=\"published\"} %llu\n",
			      (unsigned long long)mq.published_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_mq_messages_total{action=\"delivered\"} %llu\n",
			      (unsigned long long)mq.delivered_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_mq_queue_depth %llu\n",
			      (unsigned long long)mq.queue_depth);
	}

	/* AI Stats */
	csilk_ai_stats_t ai;
	csilk_ai_get_stats(&ai);
	if (ai.requests_total > 0) {
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_ai_requests_total %llu\n",
			      (unsigned long long)ai.requests_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_ai_tokens_total %llu\n",
			      (unsigned long long)ai.tokens_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_ai_errors_total %llu\n",
			      (unsigned long long)ai.errors_total);
	}

	/* DB Stats */
	csilk_db_stats_t db;
	csilk_db_get_stats(&db);
	if (db.queries_total + db.execs_total > 0) {
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_db_operations_total{type=\"query\"} %llu\n",
			      (unsigned long long)db.queries_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_db_operations_total{type=\"exec\"} %llu\n",
			      (unsigned long long)db.execs_total);
		append_metric(&buf,
			      &size,
			      &offset,
			      "csilk_db_errors_total %llu\n",
			      (unsigned long long)db.errors_total);
	}

	/* Set response headers and body */
	csilk_set_header(c, "Content-Type", "text/plain; version=0.0.4");

	/* Wrap the dynamically allocated buffer in the response.
     The framework will automatically call free() when the response is sent
     because body_is_managed is set to 1. */
	csilk_set_response_body(c, buf, offset, 1);
	csilk_status(c, CSILK_STATUS_OK);
}

uint64_t
csilk_metrics_get_total_requests(void)
{
	return atomic_load(&http_requests_total);
}

uint64_t
csilk_metrics_get_total_duration(void)
{
	return atomic_load(&http_request_duration_microseconds);
}
