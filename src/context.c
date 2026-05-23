#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gin.h"

/** @brief Move to the next handler in the chain.
 * @param c The request context. */
void gin_next(gin_ctx_t* c) {
  if (c->aborted) return;
  c->handler_index++;
  if (c->handlers[c->handler_index] != NULL) {
    c->handlers[c->handler_index](c);
  }
}

/** @brief Abort the handler chain execution.
 * @param c The request context. */
void gin_abort(gin_ctx_t* c) { c->aborted = 1; }

/** @brief Set the response status code.
 * @param c The request context.
 * @param status The HTTP status code. */
void gin_status(gin_ctx_t* c, int status) { c->response.status = status; }

/** @brief Set response body and status code.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param msg The response body message. */
void gin_string(gin_ctx_t* c, int status, const char* msg) {
  c->response.status = status;
  if (c->arena) {
    c->response.body = msg ? gin_arena_strdup(c->arena, msg) : NULL;
    c->response.body_is_managed = 0; // Managed by arena
  } else {
    if (c->response.body && c->response.body_is_managed) {
      free((void*)c->response.body);
    }
    c->response.body = msg ? strdup(msg) : NULL;
    c->response.body_is_managed = 1;
  }
}

/** @brief Get a URL parameter by key.
 * @param c The request context.
 * @param key The parameter key.
 * @return The parameter value, or NULL if not found. */
const char* gin_get_param(gin_ctx_t* c, const char* key) {
  for (int i = 0; i < c->params_count; i++) {
    if (strcmp(c->params[i].key, key) == 0) {
      return c->params[i].value;
    }
  }
  return NULL;
}

/** @brief Get a request header by key.
 * @param c The request context.
 * @param key The header key.
 * @return The header value, or NULL if not found. */
const char* gin_get_header(gin_ctx_t* c, const char* key) {
  gin_header_t* h = c->request.headers;
  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      return h->value;
    }
    h = h->next;
  }
  return NULL;
}

/** @brief Get a query parameter by key.
 * @param c The request context.
 * @param key The query parameter key.
 * @return The query parameter value, or NULL if not found. */
const char* gin_get_query(gin_ctx_t* c, const char* key) {
  gin_header_t* h = c->request.query_params;
  while (h) {
    if (strcmp(h->key, key) == 0) {
      return h->value;
    }
    h = h->next;
  }
  return NULL;
}

void gin_set_request_header(gin_ctx_t* c, const char* key, const char* value) {
  gin_header_t* h = c->request.headers;
  gin_header_t* prev = NULL;

  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      char* new_val = strdup(value);
      if (!new_val) return;
      free(h->value);
      h->value = new_val;
      return;
    }
    prev = h;
    h = h->next;
  }

  gin_header_t* new_h = malloc(sizeof(gin_header_t));
  if (!new_h) return;

  new_h->key = strdup(key);
  if (!new_h->key) {
    free(new_h);
    return;
  }

  new_h->value = strdup(value);
  if (!new_h->value) {
    free(new_h->key);
    free(new_h);
    return;
  }

  new_h->next = NULL;

  if (prev) {
    prev->next = new_h;
  } else {
    c->request.headers = new_h;
  }
}

/** @brief Set a response header.
 * @param c The request context.
 * @param key The header key.
 * @param value The header value. */
void gin_set_header(gin_ctx_t* c, const char* key, const char* value) {
  gin_header_t* h = c->response.headers;
  gin_header_t* prev = NULL;

  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      char* new_val = strdup(value);
      if (!new_val) return;
      free(h->value);
      h->value = new_val;
      return;
    }
    prev = h;
    h = h->next;
  }

  gin_header_t* new_h = malloc(sizeof(gin_header_t));
  if (!new_h) return;

  new_h->key = strdup(key);
  if (!new_h->key) {
    free(new_h);
    return;
  }

  new_h->value = strdup(value);
  if (!new_h->value) {
    free(new_h->key);
    free(new_h);
    return;
  }

  new_h->next = NULL;

  if (prev) {
    prev->next = new_h;
  } else {
    c->response.headers = new_h;
  }
}

/** @brief Internal helper to free header list.
 * @param h The head of the header list. */
static void free_headers(gin_header_t* h) {
  while (h) {
    gin_header_t* next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }
}

/** @brief Clean up context resources.
 * @param c The request context. */
void gin_ctx_cleanup(gin_ctx_t* c) {
  if (!c) return;
  
  if (c->arena) {
    gin_arena_free(c->arena);
    c->arena = NULL;
  }

  for (int i = 0; i < c->params_count; i++) {
    free(c->params[i].key);
    free(c->params[i].value);
  }
  c->params_count = 0;

  if (c->request.body) {
    free(c->request.body);
    c->request.body = NULL;
  }

  free_headers(c->request.headers);
  c->request.headers = NULL;
  free_headers(c->request.query_params);
  c->request.query_params = NULL;
  free_headers(c->response.headers);
  c->response.headers = NULL;

  if (c->response.body && c->response.body_is_managed) {
    free((void*)c->response.body);
    c->response.body = NULL;
  }
}

/** @brief Bind request body to JSON.
 * @param c The request context.
 * @return A cJSON pointer representing the parsed body, or NULL. */
cJSON* gin_bind_json(gin_ctx_t* c) {
  if (!c || !c->request.body) return NULL;
  return cJSON_Parse(c->request.body);
}

/** @brief Bind request body to JSON with error feedback.
 * @param c The request context.
 * @param error Optional pointer to store error message (NULL if no error).
 * @return A cJSON pointer, or NULL on failure (check *error for details). */
cJSON* gin_bind_json_err(gin_ctx_t* c, const char** error) {
  if (error) *error = NULL;
  if (!c) {
    if (error) *error = "Null context";
    return NULL;
  }
  if (!c->request.body) {
    if (error) *error = "No request body";
    return NULL;
  }
  cJSON* json = cJSON_Parse(c->request.body);
  if (!json) {
    if (error) *error = cJSON_GetErrorPtr();
    if (error && !*error) *error = "Invalid JSON";
    return NULL;
  }
  return json;
}

/** @brief Set JSON response body and status.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param json The cJSON pointer to send as response. */
void gin_json(gin_ctx_t* c, int status, cJSON* json) {
  if (!c || !json) return;

  c->response.status = status;
  gin_set_header(c, "Content-Type", "application/json");
  c->response.body = cJSON_PrintUnformatted(json);
  c->response.body_is_managed = 1;
  cJSON_Delete(json);
}

/** @brief Set a JSON error response with message.
 * @param c The request context.
 * @param status The HTTP status code.
 * @param message The error message string. */
void gin_json_error(gin_ctx_t* c, int status, const char* message) {
  if (!c) return;
  cJSON* err = cJSON_CreateObject();
  if (!err) return;
  cJSON_AddStringToObject(err, "error", message ? message : "Unknown error");
  gin_json(c, status, err);
}
