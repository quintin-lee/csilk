/**
 * @file request_id.c
 * @brief Request ID middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "csilk/core/context_internal.h"
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
void csilk_request_id_middleware(csilk_ctx_t* c) {
  if (!c) return;

  /* Generate a UUID v4 if the request does not already carry one.
     The UUID is stored in the context's request_id field (36 chars + null)
     and reused across the entire request lifecycle. */
  if (c->request_id[0] == '\0') {
    _csilk_generate_uuid(c, c->request_id);
  }

  /* Propagate as response header for client-side request tracking
     and as a thread-local logger context for log correlation. */
  csilk_set_header(c, "X-Request-Id", c->request_id);

  csilk_log_set_request_id(c->request_id);

  csilk_next(c);
}

/**
 * @brief Built-in Health Check handler.
 *
 * Returns a simple JSON response with status 200 OK and body
 * `{"status":"up"}`. Useful for load balancer health probes and
 * container orchestration liveness/readiness checks.
 *
 * @param c  The request context. If NULL the function returns immediately.
 */
void csilk_health_check_handler(csilk_ctx_t* c) {
  if (!c) return;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "up");
  csilk_json(c, CSILK_STATUS_OK, root);
}
