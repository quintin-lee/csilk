#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "gin.h"

struct gin_server_s {
  uv_loop_t* loop;
  gin_router_t* router;
  uv_tcp_t server_handle;
  llhttp_settings_t settings;
};

typedef struct {
  uv_tcp_t handle;
  uv_timer_t timer;
  llhttp_t parser;
  gin_server_t* server;
  gin_ctx_t ctx;
  char* current_url;
  char* current_header_field;
  char* current_header_value;
} gin_client_t;

static void alloc_buffer(uv_handle_t* handle, size_t suggested_size,
                         uv_buf_t* buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  if (buf->base == NULL) {
    buf->len = 0;
  } else {
    buf->len = suggested_size;
  }
}

/**
 * @brief Close handler for client connections.
 * @param handle UV handle associated with client connection.
 */
static void on_close(uv_handle_t* handle) {
  gin_client_t* client = (gin_client_t*)handle->data;
  gin_ctx_cleanup(&client->ctx);
  if (client->current_url) {
    free(client->current_url);
  }
  if (client->current_header_field) {
    free(client->current_header_field);
  }
  if (client->current_header_value) {
    free(client->current_header_value);
  }
  free(client);
}

/**
 * @brief Write completion callback.
 * @param req Write request.
 * @param status Write status.
 */
static void on_write(uv_write_t* req, int status) {
  if (status) {
    fprintf(stderr, "Write error %s\n", uv_strerror(status));
  }
  free(req->data);
  free(req);
}

/**
 * @brief Timeout callback for client connections.
 * @param handle Timer handle.
 */
static void on_timeout(uv_timer_t* handle) {
  gin_client_t* client = (gin_client_t*)handle->data;
  if (!uv_is_closing((uv_handle_t*)&client->handle)) {
    uv_close((uv_handle_t*)&client->handle, on_close);
  }
}

/**
 * @brief Set a request header in the context.
 * @param c Gin context.
 * @param key Header key.
 * @param value Header value.
 */
static void gin_set_request_header(gin_ctx_t* c, const char* key,
                                    const char* value) {
  gin_header_t* h = malloc(sizeof(gin_header_t));
  if (h) {
    h->key = strdup(key);
    h->value = strdup(value);
    if (!h->key || !h->value) {
      if (h->key) free(h->key);
      if (h->value) free(h->value);
      free(h);
      return;
    }
    h->next = c->request.headers;
    c->request.headers = h;
  }
}

/**
 * @brief Append a chunk of data to a string.
 * @param str Pointer to string pointer.
 * @param at Data to append.
 * @param length Length of data to append.
 * @return 0 on success, HPE_USER on allocation failure.
 */
static int append_chunk(char** str, const char* at, size_t length) {
  if (*str == NULL) {
    *str = malloc(length + 1);
    if (!*str) return HPE_USER;
    memcpy(*str, at, length);
    (*str)[length] = '\0';
  } else {
    size_t old_len = strlen(*str);
    char* new_str = realloc(*str, old_len + length + 1);
    if (!new_str) return HPE_USER;
    *str = new_str;
    memcpy(*str + old_len, at, length);
    (*str)[old_len + length] = '\0';
  }
  return 0;
}

/**
 * @brief HTTP URL parser callback.
 * @param p HTTP parser instance.
 * @param at Pointer to URL data.
 * @param length Length of URL data.
 * @return 0 on success, HPE_USER on error.
 */
static int on_url(llhttp_t* p, const char* at, size_t length) {
  gin_client_t* client = (gin_client_t*)p->data;
  if (append_chunk(&client->current_url, at, length) != 0) {
    return HPE_USER;
  }
  return 0;
}

/**
 * @brief HTTP header field parser callback.
 * @param p HTTP parser instance.
 * @param at Pointer to header field data.
 * @param length Length of header field data.
 * @return 0 on success, HPE_USER on error.
 */
static int on_header_field(llhttp_t* p, const char* at, size_t length) {
  gin_client_t* client = (gin_client_t*)p->data;
  if (client->current_header_value) {
    gin_set_request_header(&client->ctx, client->current_header_field,
                            client->current_header_value);
    free(client->current_header_field);
    free(client->current_header_value);
    client->current_header_field = NULL;
    client->current_header_value = NULL;
  }
  if (append_chunk(&client->current_header_field, at, length) != 0) {
    return HPE_USER;
  }
  return 0;
}

/**
 * @brief HTTP header value parser callback.
 * @param p HTTP parser instance.
 * @param at Pointer to header value data.
 * @param length Length of header value data.
 * @return 0 on success, HPE_USER on error.
 */
static int on_header_value(llhttp_t* p, const char* at, size_t length) {
  gin_client_t* client = (gin_client_t*)p->data;
  if (append_chunk(&client->current_header_value, at, length) != 0) {
    return HPE_USER;
  }
  return 0;
}

/**
 * @brief HTTP body parser callback.
 * @param p HTTP parser instance.
 * @param at Pointer to body data.
 * @param length Length of body data.
 * @return 0 on success, HPE_USER on error.
 */
static int on_body(llhttp_t* p, const char* at, size_t length) {
  gin_client_t* client = (gin_client_t*)p->data;
  if (client->ctx.request.body_len + length > 1024 * 1024) {
    return HPE_USER;
  }

  char* new_body = realloc(client->ctx.request.body,
                            client->ctx.request.body_len + length + 1);
  if (!new_body) {
    return HPE_USER;
  }
  client->ctx.request.body = new_body;
  memcpy(client->ctx.request.body + client->ctx.request.body_len, at, length);
  client->ctx.request.body_len += length;
  client->ctx.request.body[client->ctx.request.body_len] = '\0';

  return 0;
}

static int on_message_complete(llhttp_t* p) {
  gin_client_t* client = (gin_client_t*)p->data;

  if (client->current_url) {
    char* path = NULL;
    char* query = NULL;
    gin_split_url(client->current_url, &path, &query);
    client->ctx.request.path = path;
    if (query) {
      gin_parse_query(&client->ctx, query);
      free(query);
    }
    free(client->current_url);
    client->current_url = NULL;
  }

  if (client->current_header_field && client->current_header_value) {
    gin_set_request_header(&client->ctx, client->current_header_field,
                           client->current_header_value);
    free(client->current_header_field);
    free(client->current_header_value);
    client->current_header_field = NULL;
    client->current_header_value = NULL;
  }

  client->ctx.request.method = (char*)llhttp_method_name(llhttp_get_method(p));

  if (gin_router_match_ctx(client->server->router, &client->ctx)) {
    gin_next(&client->ctx);
  } else {
    gin_string(&client->ctx, 404, "Not Found");
  }

  int status = client->ctx.response.status ? client->ctx.response.status : 200;
  const char* status_text = "OK";
  switch (status) {
    case 200:
      status_text = "OK";
      break;
    case 201:
      status_text = "Created";
      break;
    case 204:
      status_text = "No Content";
      break;
    case 400:
      status_text = "Bad Request";
      break;
    case 401:
      status_text = "Unauthorized";
      break;
    case 403:
      status_text = "Forbidden";
      break;
    case 404:
      status_text = "Not Found";
      break;
    case 500:
      status_text = "Internal Server Error";
      break;
    default:
      status_text = "OK";
      break;
  }

  size_t body_len =
      client->ctx.response.body ? strlen(client->ctx.response.body) : 0;
  const char* body = client->ctx.response.body ? client->ctx.response.body : "";

  int keep_alive = llhttp_should_keep_alive(p);

  int len = snprintf(NULL, 0,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n"
                     "%s",
                     status, status_text, body_len,
                     keep_alive ? "keep-alive" : "close", body);

  if (len < 0) {
    return 0;
  }

  uv_write_t* req = malloc(sizeof(uv_write_t));
  if (req) {
    char* write_base = malloc(len + 1);
    if (write_base) {
      snprintf(write_base, len + 1,
               "HTTP/1.1 %d %s\r\n"
               "Content-Length: %zu\r\n"
               "Connection: %s\r\n"
               "\r\n"
               "%s",
               status, status_text, body_len,
               keep_alive ? "keep-alive" : "close", body);
      uv_buf_t buf = uv_buf_init(write_base, len);
      req->data = write_base;
      uv_write(req, (uv_stream_t*)&client->handle, &buf, 1, on_write);
    } else {
      free(req);
    }
  }

  if (keep_alive) {
    llhttp_reset(p);
    uv_timer_start(&client->timer, on_timeout, 5000, 0);
  } else {
    if (!uv_is_closing((uv_handle_t*)&client->handle)) {
      uv_close((uv_handle_t*)&client->handle, on_close);
    }
  }

  return 0;
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  gin_client_t* client = (gin_client_t*)stream->data;
  uv_timer_stop(&client->timer);

  if (nread > 0) {
    enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);
    if (err != HPE_OK) {
      fprintf(stderr, "Parse error: %s %s\n", llhttp_errno_name(err),
              client->parser.reason);
      if (!uv_is_closing((uv_handle_t*)stream)) {
        uv_close((uv_handle_t*)stream, on_close);
      }
    }
  } else if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "Read error %s\n", uv_err_name(nread));
    }
    if (!uv_is_closing((uv_handle_t*)stream)) {
      uv_close((uv_handle_t*)stream, on_close);
    }
  }

  if (buf->base) {
    free(buf->base);
  }
}

static void on_new_connection(uv_stream_t* server_stream, int status) {
  if (status < 0) {
    fprintf(stderr, "New connection error %s\n", uv_strerror(status));
    return;
  }

  gin_server_t* server = (gin_server_t*)server_stream->data;
  gin_client_t* client = calloc(1, sizeof(gin_client_t));
  if (!client) return;

  client->server = server;
  client->parser.data = client;
  int r = uv_tcp_init(server->loop, &client->handle);
  if (r < 0) {
    fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
    free(client);
    return;
  }
  client->handle.data = client;

  if (uv_accept(server_stream, (uv_stream_t*)&client->handle) == 0) {
    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    uv_timer_init(server->loop, &client->timer);
    client->timer.data = client;

    r = uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
    if (r < 0) {
      fprintf(stderr, "uv_read_start error %s\n", uv_strerror(r));
      if (!uv_is_closing((uv_handle_t*)&client->handle)) {
        uv_close((uv_handle_t*)&client->handle, on_close);
      }
    }
  } else {
    if (!uv_is_closing((uv_handle_t*)&client->handle)) {
      uv_close((uv_handle_t*)&client->handle, on_close);
    }
  }
}

gin_server_t* gin_server_new(gin_router_t* router) {
  gin_server_t* s = calloc(1, sizeof(gin_server_t));
  if (!s) return NULL;
  s->loop = uv_default_loop();
  if (!s->loop) {
    free(s);
    return NULL;
  }
  s->router = router;
  llhttp_settings_init(&s->settings);
  s->settings.on_url = on_url;
  s->settings.on_header_field = on_header_field;
  s->settings.on_header_value = on_header_value;
  s->settings.on_body = on_body;
  s->settings.on_message_complete = on_message_complete;
  return s;
}

void gin_server_free(gin_server_t* server) {
  if (server) {
    if (server->server_handle.loop &&
        !uv_is_closing((uv_handle_t*)&server->server_handle)) {
      uv_close((uv_handle_t*)&server->server_handle, NULL);
    }
    free(server);
  }
}

int gin_server_run(gin_server_t* server, int port) {
  int r = uv_tcp_init(server->loop, &server->server_handle);
  if (r < 0) {
    fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
    return r;
  }
  server->server_handle.data = server;

  struct sockaddr_in addr;
  r = uv_ip4_addr("0.0.0.0", port, &addr);
  if (r < 0) {
    fprintf(stderr, "uv_ip4_addr error %s\n", uv_strerror(r));
    return r;
  }

  r = uv_tcp_bind(&server->server_handle, (const struct sockaddr*)&addr, 0);
  if (r < 0) {
    fprintf(stderr, "uv_tcp_bind error %s\n", uv_strerror(r));
    return r;
  }

  r = uv_listen((uv_stream_t*)&server->server_handle, 128, on_new_connection);
  if (r < 0) {
    fprintf(stderr, "uv_listen error %s\n", uv_strerror(r));
    return r;
  }

  return uv_run(server->loop, UV_RUN_DEFAULT);
}
