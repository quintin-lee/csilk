/**
 * @file request_id.c
 * @brief Request ID middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/**
 * @brief Request ID middleware handler.
 *
 * Ensures every request has a unique UUID v4 identifier. If the request
 * context does not already have an ID (c->request_id is empty), a new UUID
 * is generated via _csilk_generate_uuid(). The identifier is then set as
 * the "X-Request-Id" response header and propagated to the thread-local
 * logger state for distributed tracing correlation.
 *
 * @param c  The request context. If NULL the function returns immediately.
 *
 * @note Should be registered as the very first middleware in the pipeline
 *       so all downstream handlers and log entries share the same request
 *       identifier.
 * @warning The UUID generation uses whatever _csilk_generate_uuid()
 *          implements — ensure it provides sufficient entropy for your
 *          deployment.
 */
void
csilk_request_id_middleware(csilk_ctx_t* c)
{
	if (!c) {
		return;
	}

	/* Generate a UUID v4 if the request does not already carry one.
     The UUID is stored in the context's request_id field (36 chars + null)
     and reused across the entire request lifecycle. */
	const char* rid = csilk_get_request_id(c);
	if (!rid || rid[0] == '\0') {
		char buf[37];
		_csilk_generate_uuid(c, buf);
		csilk_set_request_id(c, buf);
		rid = csilk_get_request_id(c);
	}

	/* Propagate as response header for client-side request tracking
     and as a thread-local logger context for log correlation. */
	csilk_set_header(c, "X-Request-Id", rid);

	csilk_log_set_request_id(rid);

	csilk_next(c);
}

/**
 * @brief Built-in Health Check handler (Liveness).
 *
 * Provides a simple "shallow" check to verify that the server's event loop
 * is alive and responsive. Returns a JSON response with status 200 OK.
 *
 * @param c  The request context. If NULL the function returns immediately.
 * @note Used by load balancers and orchestrators for Liveness probes.
 */
void
csilk_health_check_handler(csilk_ctx_t* c)
{
	if (!c) {
		return;
	}

	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status", "up");
	csilk_json(c, CSILK_STATUS_OK, root);
}

/**
 * @brief Built-in Readiness Check handler (Readiness).
 *
 * Performs a "deep" check of the server's health by inspecting critical
 * subsystems. It checks:
 * 1. MQ Queue Depth: Returns unhealthy if the message queue is saturated.
 * 2. Active Connections: Returns unhealthy if the server is near its capacity.
 *
 * Returns 200 OK if the server is ready to accept more traffic, or
 * 503 Service Unavailable if any check fails.
 *
 * @param c  The request context. If NULL the function returns immediately.
 * @note Used by load balancers and orchestrators for Readiness probes to
 *       decide whether to route traffic to this instance.
 */
void
csilk_ready_check_handler(csilk_ctx_t* c)
{
	if (!c) {
		return;
	}

	int is_healthy = 1;
	const char* reason = NULL;

	/* 1. Check MQ Health */
	csilk_mq_stats_t mq;
	csilk_mq_get_stats(csilk_server_get_mq(csilk_ctx_get_server(c)), &mq);
	if (mq.queue_depth > 1000) { /* Arbitrary threshold for saturation */
		is_healthy = 0;
		reason = "mq_saturated";
	}

	/* 2. Check Connection Health */
	int active = 0, pooled = 0;
	csilk_server_get_stats(csilk_ctx_get_server(c), &active, &pooled);
	if (active > 5000) { /* Threshold for connection limit */
		is_healthy = 0;
		reason = "connection_limit_reached";
	}

	cJSON* root = cJSON_CreateObject();
	if (is_healthy) {
		cJSON_AddStringToObject(root, "status", "ready");
		csilk_json(c, CSILK_STATUS_OK, root);
	} else {
		cJSON_AddStringToObject(root, "status", "unavailable");
		cJSON_AddStringToObject(root, "reason", reason);
		csilk_json(c, CSILK_STATUS_SERVICE_UNAVAILABLE, root);
	}
}
