/**
 * @file context.c
 * @brief Request/response context implementation.
 * @copyright MIT License
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "csilk.h"
#include "csilk_internal.h"
#include <uv.h>

/** @brief Hash a header key string into a bucket index.
 * Uses djb2 hash with case-insensitive folding.
 * @param key Header key string.
 * @return Bucket index (0..CSILK_HEADER_BUCKETS-1). */
static uint32_t hash_key(const char *key) {
  uint32_t hash = 5381;
  int c;
  while ((c = (unsigned char)*key++)) {
    hash = ((hash << 5) + hash) + tolower(c);
  }
  return hash % CSILK_HEADER_BUCKETS;
}

/** @brief Look up a header value by key (case-insensitive).
 * @param map Header hash map.
 * @param key Header key to find.
 * @return Value string, or NULL if not found. */
static const char *map_get(csilk_header_map_t *map, const char *key) {
  uint32_t bucket = hash_key(key);
  csilk_header_t *h = map->buckets[bucket];
  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      return h->value;
    }
    h = h->next;
  }
  return NULL;
}

/** @brief Set a header value (overwrites existing key).
 * @param c Request context (for arena allocator).
 * @param map Header hash map (request or response).
 * @param key Header key.
 * @param value Header value. */
static void map_set(csilk_ctx_t *c, csilk_header_map_t *map, const char *key,
                    const char *value) {
  if (!c->arena)
    return;
  uint32_t bucket = hash_key(key);
  csilk_header_t *h = map->buckets[bucket];
  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      h->value = csilk_arena_strdup(c->arena, value);
      return;
    }
    h = h->next;
  }

  csilk_header_t *new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
  if (new_h) {
    new_h->key = csilk_arena_strdup(c->arena, key);
    new_h->value = csilk_arena_strdup(c->arena, value);
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;
  }
}

/** @brief Add a header value (allows duplicates).
 * @param c Request context (for arena allocator).
 * @param map Header hash map.
 * @param key Header key.
 * @param value Header value. */
static void map_add(csilk_ctx_t *c, csilk_header_map_t *map, const char *key,
                    const char *value) {
  if (!c->arena)
    return;
  uint32_t bucket = hash_key(key);
  csilk_header_t *new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
  if (new_h) {
    new_h->key = csilk_arena_strdup(c->arena, key);
    new_h->value = csilk_arena_strdup(c->arena, value);
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;
  }
}

/** @brief Advance to the next handler in the chain. */
void csilk_next(csilk_ctx_t *c) {
  if (c->aborted)
    return;
  c->handler_index++;
  if (c->handlers[c->handler_index] != NULL) {
    c->handlers[c->handler_index](c);
  }
}

/** @brief Abort the handler chain immediately. */
void csilk_abort(csilk_ctx_t *c) { c->aborted = 1; }

/** @brief Set the HTTP response status code. */
void csilk_status(csilk_ctx_t *c, int status) { c->response.status = status; }

/** @brief Set response body as plain text with status code.
 * Memory handled by arena if available, else strdup. */
void csilk_string(csilk_ctx_t *c, int status, const char *msg) {
  c->response.status = status;
  size_t msg_len = msg ? strlen(msg) : 0;
  if (c->arena) {
    c->response.body = msg ? csilk_arena_strdup(c->arena, msg) : NULL;
    c->response.body_len = msg_len;
    c->response.body_is_managed = 0;
  } else {
    if (c->response.body && c->response.body_is_managed) {
      free((void *)c->response.body);
    }
    char *body = msg ? strdup(msg) : NULL;
    c->response.body = body;
    c->response.body_len = body ? msg_len : 0;
    c->response.body_is_managed = body ? 1 : 0;
  }
}

/** @brief Get a URL path parameter by name. */
const char *csilk_get_param(csilk_ctx_t *c, const char *key) {
  for (int i = 0; i < c->params_count; i++) {
    if (strcmp(c->params[i].key, key) == 0) {
      return c->params[i].value;
    }
  }
  return NULL;
}

/** @brief Get a request header value (case-insensitive). */
const char *csilk_get_header(csilk_ctx_t *c, const char *key) {
  return map_get(&c->request.headers, key);
}

/** @brief Get a query parameter value. */
const char *csilk_get_query(csilk_ctx_t *c, const char *key) {
  return map_get(&c->request.query_params, key);
}

/** @brief Set a request header (overwrites existing). */
void csilk_set_request_header(csilk_ctx_t *c, const char *key,
                              const char *value) {
  map_set(c, &c->request.headers, key, value);
}

/** @brief Set a response header (overwrites existing). */
void csilk_set_header(csilk_ctx_t *c, const char *key, const char *value) {
  map_set(c, &c->response.headers, key, value);
}

/** @brief Clean up request context resources between requests.
 * Resets arena, frees params/body/path, clears headers and storage. */
void csilk_ctx_cleanup(csilk_ctx_t *c) {
  if (!c)
    return;

  if (c->arena) {
    csilk_arena_reset(c->arena);
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

  if (c->request.path) {
    free(c->request.path);
    c->request.path = NULL;
  }

  memset(&c->request.headers, 0, sizeof(csilk_header_map_t));
  memset(&c->request.query_params, 0, sizeof(csilk_header_map_t));
  memset(&c->response.headers, 0, sizeof(csilk_header_map_t));

  if (c->response.body && c->response.body_is_managed) {
    free((void *)c->response.body);
    c->response.body = NULL;
    c->response.body_is_managed = 0;
  }

  c->storage_head = NULL;

  c->aborted = 0;
  c->is_websocket = 0;
  c->is_sse = 0;
  c->is_async = 0;
  c->response_started = 0;
  c->handler_index = -1;
}

const char *csilk_get_method(csilk_ctx_t *c) {
  return c ? c->request.method : NULL;
}

const char *csilk_get_path(csilk_ctx_t *c) {
  return c ? c->request.path : NULL;
}

const char *csilk_get_body(csilk_ctx_t *c, size_t *out_len) {
  if (out_len)
    *out_len = c ? c->request.body_len : 0;
  return c ? c->request.body : NULL;
}

size_t csilk_get_body_len(csilk_ctx_t *c) {
  return c ? c->request.body_len : 0;
}

int csilk_is_websocket(csilk_ctx_t *c) { return c ? c->is_websocket : 0; }

int csilk_is_sse(csilk_ctx_t *c) { return c ? c->is_sse : 0; }

int csilk_is_aborted(csilk_ctx_t *c) { return c ? c->aborted : 0; }

void csilk_set_on_ws_message(csilk_ctx_t *c,
                             void (*cb)(csilk_ctx_t *c, const uint8_t *payload,
                                        size_t len, int opcode)) {
  if (c)
    c->on_ws_message = cb;
}

/** @brief Redirect to another URL with custom status. */
void csilk_redirect(csilk_ctx_t* c, int status, const char* location) {
    if (!c || !location) return;
    csilk_set_header(c, "Location", location);
    csilk_string(c, status, "Redirecting...");
    csilk_abort(c);
}

/** @brief Redirect to another URL (default 302). */
void csilk_redirect_simple(csilk_ctx_t* c, const char* url) {
    csilk_redirect(c, 302, url);
}

/** @brief Store a value in the context storage. */
void csilk_set(csilk_ctx_t *c, const char *key, void *value) {
  if (!c || !key || !c->arena)
    return;

  csilk_storage_item_t *item = c->storage_head;
  while (item) {
    if (strcmp(item->key, key) == 0) {
      item->value = value;
      return;
    }
    item = item->next;
  }

  csilk_storage_item_t *new_item =
      csilk_arena_alloc(c->arena, sizeof(csilk_storage_item_t));
  if (new_item) {
    new_item->key = csilk_arena_strdup(c->arena, key);
    new_item->value = value;
    new_item->next = c->storage_head;
    c->storage_head = new_item;
  }
}

/** @brief Retrieve a value from the context storage. */
void *csilk_get(csilk_ctx_t *c, const char *key) {
  if (!c || !key)
    return NULL;
  csilk_storage_item_t *item = c->storage_head;
  while (item) {
    if (strcmp(item->key, key) == 0) {
      return item->value;
    }
    item = item->next;
  }
  return NULL;
}

/** @brief Parse request body as JSON. */
cJSON *csilk_bind_json(csilk_ctx_t *c) {
  if (!c || !c->request.body)
    return NULL;
  return cJSON_Parse(c->request.body);
}

/** @brief Parse request body as JSON with detailed error feedback. */
cJSON *csilk_bind_json_err(csilk_ctx_t *c, const char **error) {
  if (error)
    *error = NULL;
  if (!c) {
    if (error)
      *error = "Null context";
    return NULL;
  }
  if (!c->request.body) {
    if (error)
      *error = "No request body";
    return NULL;
  }
  cJSON *json = cJSON_Parse(c->request.body);
  if (!json) {
    if (error)
      *error = cJSON_GetErrorPtr();
    if (error && !*error)
      *error = "Invalid JSON";
    return NULL;
  }
  return json;
}

/** @brief Get a cookie value by name. */
const char *csilk_get_cookie(csilk_ctx_t *c, const char *name) {
  if (!c || !name || !c->arena)
    return NULL;
  const char *cookie_header = csilk_get_header(c, "Cookie");
  if (!cookie_header)
    return NULL;

  char *cookies = strdup(cookie_header);
  if (!cookies)
    return NULL;

  char *saveptr;
  char *cookie = strtok_r(cookies, "; ", &saveptr);
  const char *result = NULL;

  while (cookie) {
    char *eq = strchr(cookie, '=');
    if (eq) {
      *eq = '\0';
      if (strcmp(cookie, name) == 0) {
        // Found it! Use arena to store the result so it survives the function
        // call
        result = csilk_arena_strdup(c->arena, eq + 1);
        break;
      }
    }
    cookie = strtok_r(NULL, "; ", &saveptr);
  }

  free(cookies);
  return result;
}

/** @brief Add a response header (allows multiple values for same key). */
void csilk_add_header(csilk_ctx_t *c, const char *key, const char *value) {
  map_add(c, &c->response.headers, key, value);
}

/** @brief Set a cookie in the response. */
void csilk_set_cookie(csilk_ctx_t *c, const char *name, const char *value,
                      int max_age, const char *path, const char *domain,
                      int secure, int http_only) {
  if (!c->arena)
    return;
  size_t buf_size = strlen(name) + strlen(value) + 256; // 256 for attributes
  if (path)
    buf_size += strlen(path);
  if (domain)
    buf_size += strlen(domain);

  char *buf = csilk_arena_alloc(c->arena, buf_size);
  if (!buf)
    return;

  int pos = snprintf(buf, buf_size, "%s=%s", name, value);

  if (max_age > 0) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Max-Age=%d", max_age);
  } else if (max_age < 0) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Max-Age=0");
  }

  if (path) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Path=%s", path);
  } else {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Path=/");
  }

  if (domain) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Domain=%s", domain);
  }

  if (secure) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; Secure");
  }

  if (http_only) {
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "; HttpOnly");
  }

  csilk_add_header(c, "Set-Cookie", buf);
}

/** @brief Send a JSON response (cJSON object is freed by this call). */
void csilk_json(csilk_ctx_t *c, int status, cJSON *json) {
  if (!c || !json)
    return;

  c->response.status = status;
  csilk_set_header(c, "Content-Type", "application/json");

  if (c->response.body && c->response.body_is_managed) {
    free((void *)c->response.body);
    c->response.body = NULL;
    c->response.body_is_managed = 0;
  }

  char *body = cJSON_PrintUnformatted(json);
  if (body) {
    c->response.body = body;
    c->response.body_len = strlen(body);
    c->response.body_is_managed = 1;
  }
  cJSON_Delete(json);
}

/** @brief Send a JSON error response with "error" field. */
void csilk_json_error(csilk_ctx_t *c, int status, const char *message) {
  if (!c)
    return;
  cJSON *err = cJSON_CreateObject();
  if (!err)
    return;
  cJSON_AddStringToObject(err, "error", message ? message : "Unknown error");
  csilk_json(c, status, err);
}

/** @brief Bind request body JSON to a registered struct via reflection. */
int csilk_bind_reflect(csilk_ctx_t *c, const char *type_name, void *ptr) {
  if (!c || !c->request.body || !type_name || !ptr)
    return 0;
  return csilk_json_unmarshal(type_name, c->request.body, ptr);
}

/** @brief Send a JSON response from a registered struct via reflection. */
void csilk_json_reflect(csilk_ctx_t *c, int status, const char *type_name,
                        const void *ptr) {
  if (!c || !type_name || !ptr)
    return;
  char *json_str = csilk_json_marshal(type_name, ptr);
  if (json_str) {
    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");
    if (c->response.body && c->response.body_is_managed) {
      free((void *)c->response.body);
    }
    c->response.body = json_str;
    c->response.body_len = strlen(json_str);
    c->response.body_is_managed = 1;
  }
}

/** @brief Parse a raw query string into the context's query_params map. */
void csilk_parse_query(csilk_ctx_t *c, const char *query_string) {
  if (!query_string || *query_string == '\0' || !c->arena)
    return;

  char *qs = strdup(query_string);
  if (!qs)
    return;

  char *pos = qs;
  while (pos && *pos) {
    char *amp = strchr(pos, '&');
    if (amp)
      *amp = '\0';

    char *eq = strchr(pos, '=');
    char *key = pos;
    char *value = NULL;

    if (eq) {
      *eq = '\0';
      value = eq + 1;
    } else {
      value = "";
    }

    if (*key != '\0') {
      csilk_url_decode(key);
      if (value && *value != '\0') {
        csilk_url_decode(value);
      }
      map_add(c, &c->request.query_params, key, value);
    }

    if (amp)
      pos = amp + 1;
    else
      pos = NULL;
  }
  free(qs);
}

/** @brief Streaming write completion callback.
 *  @param req Write request.
 *  @param status Write status. */
static void on_stream_write(uv_write_t* req, int status) {
    if (status < 0) {
        fprintf(stderr, "Stream write error %s\n", uv_strerror(status));
    }
    if (req->data) free(req->data);
    free(req);
}

/** @brief Send HTTP response headers with Transfer-Encoding: chunked.
 *  This is used by csilk_response_write on the first call.
 *  @param c Request context.
 *  @return 0 on success, -1 on error. */
static int send_chunked_headers(csilk_ctx_t* c) {
    if (!c || !c->_internal_client) return -1;

    int status = c->response.status ? c->response.status : 200;
    const char* status_text = status == 200 ? "OK" :
                              status == 201 ? "Created" :
                              status == 400 ? "Bad Request" :
                              status == 401 ? "Unauthorized" :
                              status == 403 ? "Forbidden" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "OK";

    size_t custom_headers_len = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            custom_headers_len += strlen(h->key) + 2 + strlen(h->value) + 2;
        }
    }

    int header_len = snprintf(NULL, 0,
        "HTTP/1.1 %d %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n",
        status, status_text);
    if (header_len < 0) return -1;

    size_t response_len = (size_t)header_len + custom_headers_len + 2;
    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) return -1;

    char* write_base = malloc(response_len + 1);
    if (!write_base) {
        free(req);
        return -1;
    }

    int pos = snprintf(write_base, response_len + 1,
        "HTTP/1.1 %d %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n",
        status, status_text);

    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            pos += snprintf(write_base + pos, response_len + 1 - (size_t)pos,
                           "%s: %s\r\n", h->key, h->value);
        }
    }

    snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n");

    uv_buf_t buf = uv_buf_init(write_base, (size_t)pos + 2);
    req->data = write_base;
    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;
    uv_write(req, stream, &buf, 1, on_stream_write);
    c->response_started = 1;
    return 0;
}

/** @brief Write a chunked frame in the format: hex-size\r\ndata\r\n.
 *  @param stream The uv stream to write to.
 *  @param data Chunk data.
 *  @param len Data length. */
static void write_chunk_frame(uv_stream_t* stream, const uint8_t* data, size_t len) {
    char size_buf[32];
    int size_len = snprintf(size_buf, sizeof(size_buf), "%zx\r\n", len);
    if (size_len <= 0) return;

    size_t total = (size_t)size_len + len + 2;
    char* buf = malloc(total);
    if (!buf) return;

    memcpy(buf, size_buf, (size_t)size_len);
    if (len > 0 && data) {
        memcpy(buf + (size_t)size_len, data, len);
    }
    buf[(size_t)size_len + len] = '\r';
    buf[(size_t)size_len + len + 1] = '\n';

    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) {
        free(buf);
        return;
    }

    uv_buf_t uv_buf = uv_buf_init(buf, (unsigned int)total);
    req->data = buf;
    uv_write(req, stream, &uv_buf, 1, on_stream_write);
}

/** @brief Write data to streaming response using chunked encoding. */
void csilk_response_write(csilk_ctx_t* c, const uint8_t* data, size_t len) {
    if (!c || !c->_internal_client) return;

    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;

    if (!c->response_started) {
        if (send_chunked_headers(c) != 0) return;
        c->response_started = 1;
    }

    if (len == 0) return;
    write_chunk_frame(stream, data, len);
}

/** @brief Finalize streaming response by sending the terminal chunk. */
void csilk_response_end(csilk_ctx_t* c) {
    if (!c || !c->_internal_client) return;

    uv_stream_t* stream = (uv_stream_t*)c->_internal_client;

    if (!c->response_started) {
        send_chunked_headers(c);
    }

    /* Terminal chunk: 0\r\n\r\n */
    const char* term = "0\r\n\r\n";
    uv_write_t* req = malloc(sizeof(uv_write_t));
    if (!req) return;

    char* buf = malloc(5);
    if (!buf) {
        free(req);
        return;
    }
    memcpy(buf, term, 5);

    uv_buf_t uv_buf = uv_buf_init(buf, 5);
    req->data = buf;
    uv_write(req, stream, &uv_buf, 1, on_stream_write);

    csilk_ctx_cleanup(c);
}
