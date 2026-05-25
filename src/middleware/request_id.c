/**
 * @file request_id.c
 * @brief Request ID middleware implementation.
 * @copyright MIT License
 */

#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Request ID middleware handler.
 *
 * Ensures every request has a unique UUID v4 identifier. If the request
 * does not already have one, a new UUID is generated and stored in the context.
 * The identifier is then added to the "X-Request-Id" response header and
 * set in the thread-local logger state for tracing. */
void csilk_request_id_middleware(csilk_ctx_t* c) {
  if (!c) return;

  /* Generate UUID if not already set */
  if (c->request_id[0] == '\0') {
    _csilk_generate_uuid(c, c->request_id);
  }

  /* Set X-Request-Id response header */
  csilk_set_header(c, "X-Request-Id", c->request_id);

  /* Set logger contextual request ID */
  csilk_log_set_request_id(c->request_id);

  csilk_next(c);
}

/** @brief Built-in Health Check handler.
 *
 * Returns a simple JSON response indicating the server status is "up". */
void csilk_health_check_handler(csilk_ctx_t* c) {
  if (!c) return;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "up");
  csilk_json(c, CSILK_STATUS_OK, root);
}
