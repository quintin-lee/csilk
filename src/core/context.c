/**
 * @file context.c
 * @brief Request/response context implementation.
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "csilk.h"

void csilk_next(csilk_ctx_t* c) {
  if (c->aborted) return;
  c->handler_index++;
  if (c->handlers[c->handler_index] != NULL) {
    c->handlers[c->handler_index](c);
  }
}

void csilk_abort(csilk_ctx_t* c) { c->aborted = 1; }

void csilk_status(csilk_ctx_t* c, int status) { c->response.status = status; }

void csilk_string(csilk_ctx_t* c, int status, const char* msg) {
  c->response.status = status;
  size_t msg_len = msg ? strlen(msg) : 0;
  if (c->arena) {
    c->response.body = msg ? csilk_arena_strdup(c->arena, msg) : NULL;
    c->response.body_len = msg_len;
    c->response.body_is_managed = 0;
  } else {
    if (c->response.body && c->response.body_is_managed) {
      free((void*)c->response.body);
    }
    char* body = msg ? strdup(msg) : NULL;
    c->response.body = body;
    c->response.body_len = body ? msg_len : 0;
    c->response.body_is_managed = body ? 1 : 0;
  }
}

const char* csilk_get_param(csilk_ctx_t* c, const char* key) {
  for (int i = 0; i < c->params_count; i++) {
    if (strcmp(c->params[i].key, key) == 0) {
      return c->params[i].value;
    }
  }
  return NULL;
}

const char* csilk_get_header(csilk_ctx_t* c, const char* key) {
  csilk_header_t* h = c->request.headers;
  while (h) {
    if (strcasecmp(h->key, key) == 0) {
      return h->value;
    }
    h = h->next;
  }
  return NULL;
}

const char* csilk_get_query(csilk_ctx_t* c, const char* key) {
  csilk_header_t* h = c->request.query_params;
  while (h) {
    if (strcmp(h->key, key) == 0) {
      return h->value;
    }
    h = h->next;
  }
  return NULL;
}

void csilk_set_request_header(csilk_ctx_t* c, const char* key, const char* value) {
  csilk_header_t* h = c->request.headers;
  csilk_header_t* prev = NULL;

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

  csilk_header_t* new_h = malloc(sizeof(csilk_header_t));
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

void csilk_set_header(csilk_ctx_t* c, const char* key, const char* value) {
  csilk_header_t* h = c->response.headers;
  csilk_header_t* prev = NULL;

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

  csilk_header_t* new_h = malloc(sizeof(csilk_header_t));
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
static void free_headers(csilk_header_t* h) {
  while (h) {
    csilk_header_t* next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }
}

void csilk_ctx_cleanup(csilk_ctx_t* c) {
  if (!c) return;
  
  if (c->arena) {
    csilk_arena_free(c->arena);
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

  if (c->request.path) {
    free(c->request.path);
    c->request.path = NULL;
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

  for (int i = 0; i < c->storage_count; i++) {
      free(c->storage[i].key);
  }
  c->storage_count = 0;
}

void csilk_set(csilk_ctx_t* c, const char* key, void* value) {
    if (!c || !key) return;
    for (int i = 0; i < c->storage_count; i++) {
        if (strcmp(c->storage[i].key, key) == 0) {
            c->storage[i].value = value;
            return;
        }
    }
    if (c->storage_count < CSILK_MAX_STORAGE) {
        c->storage[c->storage_count].key = strdup(key);
        c->storage[c->storage_count].value = value;
        c->storage_count++;
    }
}

void* csilk_get(csilk_ctx_t* c, const char* key) {
    if (!c || !key) return NULL;
    for (int i = 0; i < c->storage_count; i++) {
        if (strcmp(c->storage[i].key, key) == 0) {
            return c->storage[i].value;
        }
    }
    return NULL;
}

cJSON* csilk_bind_json(csilk_ctx_t* c) {
  if (!c || !c->request.body) return NULL;
  return cJSON_Parse(c->request.body);
}

cJSON* csilk_bind_json_err(csilk_ctx_t* c, const char** error) {
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

const char* csilk_get_cookie(csilk_ctx_t* c, const char* name) {
  if (!c || !name || !c->arena) return NULL;
  const char* cookie_header = csilk_get_header(c, "Cookie");
  if (!cookie_header) return NULL;

  char* cookies = strdup(cookie_header);
  if (!cookies) return NULL;

  char* saveptr;
  char* cookie = strtok_r(cookies, "; ", &saveptr);
  const char* result = NULL;

  while (cookie) {
    char* eq = strchr(cookie, '=');
    if (eq) {
      *eq = '\0';
      if (strcmp(cookie, name) == 0) {
        // Found it! Use arena to store the result so it survives the function call
        result = csilk_arena_strdup(c->arena, eq + 1);
        break;
      }
    }
    cookie = strtok_r(NULL, "; ", &saveptr);
  }

  free(cookies);
  return result;
}

void csilk_add_header(csilk_ctx_t* c, const char* key, const char* value) {
  csilk_header_t* h = c->response.headers;
  csilk_header_t* prev = NULL;

  while (h) {
    prev = h;
    h = h->next;
  }

  csilk_header_t* new_h = malloc(sizeof(csilk_header_t));
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

void csilk_set_cookie(csilk_ctx_t* c, const char* name, const char* value,
                    int max_age, const char* path, const char* domain,
                    int secure, int http_only) {
  size_t buf_size = strlen(name) + strlen(value) + 256; // 256 for attributes
  if (path) buf_size += strlen(path);
  if (domain) buf_size += strlen(domain);

  char* buf = malloc(buf_size);
  if (!buf) return;

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
  free(buf);
}

void csilk_json(csilk_ctx_t* c, int status, cJSON* json) {
  if (!c || !json) return;

  c->response.status = status;
  csilk_set_header(c, "Content-Type", "application/json");

  if (c->response.body && c->response.body_is_managed) {
    free((void*)c->response.body);
    c->response.body = NULL;
    c->response.body_is_managed = 0;
  }

  char* body = cJSON_PrintUnformatted(json);
  if (body) {
    c->response.body = body;
    c->response.body_len = strlen(body);
    c->response.body_is_managed = 1;
  }
  cJSON_Delete(json);
}

void csilk_json_error(csilk_ctx_t* c, int status, const char* message) {
  if (!c) return;
  cJSON* err = cJSON_CreateObject();
  if (!err) return;
  cJSON_AddStringToObject(err, "error", message ? message : "Unknown error");
  csilk_json(c, status, err);
}

int csilk_bind_reflect(csilk_ctx_t* c, const char* type_name, void* ptr) {
  if (!c || !c->request.body || !type_name || !ptr) return 0;
  return csilk_json_unmarshal(type_name, c->request.body, ptr);
}

void csilk_json_reflect(csilk_ctx_t* c, int status, const char* type_name, const void* ptr) {
  if (!c || !type_name || !ptr) return;
  char* json_str = csilk_json_marshal(type_name, ptr);
  if (json_str) {
    c->response.status = status;
    csilk_set_header(c, "Content-Type", "application/json");
    if (c->response.body && c->response.body_is_managed) {
      free((void*)c->response.body);
    }
    c->response.body = json_str;
    c->response.body_len = strlen(json_str);
    c->response.body_is_managed = 1;
  }
}

void csilk_redirect(csilk_ctx_t* c, int status, const char* location) {
  if (!c || !location) return;
  c->response.status = status;
  csilk_set_header(c, "Location", location);

  const char* body;
  switch (status) {
    case 301: body = "Moved Permanently"; break;
    case 302: body = "Found";             break;
    case 303: body = "See Other";         break;
    case 307: body = "Temporary Redirect";break;
    case 308: body = "Permanent Redirect";break;
    default:  body = "Redirect";          break;
  }
  csilk_string(c, status, body);
}
