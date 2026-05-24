/**
 * @file server.c
 * @brief Server implementation.
 * @copyright MIT License
 */

#include <limits.h>
#include <llhttp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk.h"
#include "csilk_internal.h"

/** @brief Default idle timeout in milliseconds. */
#define CSILK_DEFAULT_IDLE_TIMEOUT 5000
/** @brief Default maximum request body size in bytes. */
#define CSILK_DEFAULT_MAX_BODY_SIZE (1024UL * 1024UL)
/** @brief Default maximum request header size in bytes. */
#define CSILK_DEFAULT_MAX_HEADER_SIZE (64UL * 1024UL)
/** @brief Default TCP listen backlog. */
#define CSILK_DEFAULT_LISTEN_BACKLOG 128
/** @brief Default request arena chunk size. */
#define CSILK_DEFAULT_ARENA_SIZE 4096

/** @brief Forward declaration for client connection structure. */
typedef struct csilk_client_s csilk_client_t;

/** @brief Server structure. */
struct csilk_server_s {
  uv_loop_t* loop;                 /**< libuv event loop. */
  csilk_router_t* router;          /**< Associated router instance. */
  uv_tcp_t server_handle;          /**< TCP server handle. */
  uv_signal_t sig_handle;          /**< SIGINT signal handler. */
  uv_async_t async_handle;         /**< Async handle for cross-thread wakeup. */
  llhttp_settings_t settings;      /**< HTTP parser callback settings. */
  csilk_server_config_t config;    /**< Server configuration. */
  csilk_handler_t middlewares[32]; /**< Global middlewares. */
  int middleware_count;            /**< Number of global middlewares. */
  int max_connections; /**< Max concurrent connections (0=unlimited). */
  atomic_int active_connections; /**< Current connection count (atomic). */
  /* close tracking for async shutdown — see csilk_server_free */
  uv_thread_t* worker_tids; /**< Worker thread IDs (NULL if single-thread). */
  int worker_count;         /**< Number of worker threads created. */
  csilk_handler_t
      not_found_handler; /**< Custom 404 handler (NULL = default). */
  char* spa_doc_root;    /**< SPA fallback doc root (NULL = disabled). */
  csilk_client_t* client_pool[32]; /**< Connection object free list. */
  int client_pool_count;           /**< Number of free clients in pool. */
};

/** @brief Client connection structure. */
struct csilk_client_s {
  uv_tcp_t handle;             /**< libuv TCP stream handle. */
  uv_timer_t timer;            /**< Connection idle (keep-alive) timer. */
  uv_timer_t read_timer;       /**< Read timeout timer. */
  uv_timer_t write_timer;      /**< Write timeout timer. */
  uv_timer_t request_timer;    /**< Request timeout timer. */
  int close_pending;           /**< Pending close refs before freeing client. */
  llhttp_t parser;             /**< HTTP request parser. */
  csilk_server_t* server;      /**< Owning server instance. */
  csilk_ctx_t ctx;             /**< Request context for this connection. */
  size_t total_header_size;    /**< Total size of headers parsed so far. */
  size_t header_count;         /**< Number of headers parsed so far. */
  size_t current_url_capacity; /**< Allocated size of current_url. */
  size_t header_field_capacity; /**< Allocated size of current_header_field. */
  size_t header_value_capacity; /**< Allocated size of current_header_value. */
  char* current_url;            /**< Current URL being parsed. */
  char* current_header_field;   /**< Temporary header field name. */
  char* current_header_value;   /**< Temporary header field value. */
};

/** @brief Buffer allocation callback.
 * @param handle UV handle.
 * @param suggested_size Suggested buffer size.
 * @param buf Pointer to the buffer. */
static void alloc_buffer(uv_handle_t* handle, size_t suggested_size,
                         uv_buf_t* buf) {
  (void)handle;
  buf->base = (char*)malloc(suggested_size);
  buf->len = suggested_size;
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void on_server_handle_close(uv_handle_t* handle);

/** @brief Get a client from the connection pool (or allocate new). */
static csilk_client_t* pool_get(csilk_server_t* server) {
  if (server->client_pool_count > 0) {
    return server->client_pool[--server->client_pool_count];
  }
  return calloc(1, sizeof(csilk_client_t));
}

/** @brief Return a client to the connection pool (or free if pool full). */
static void pool_put(csilk_server_t* server, csilk_client_t* client) {
  memset(client, 0, sizeof(*client));
  if (server->client_pool_count < 32) {
    server->client_pool[server->client_pool_count++] = client;
  } else {
    free(client);
  }
}

/** @brief Close handler for timer handles — frees client on last close. */
static void on_timer_close(uv_handle_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (!client) return;
  client->close_pending--;
  if (client->close_pending > 0) return;

  if (client->server) atomic_fetch_sub(&client->server->active_connections, 1);
  csilk_ctx_cleanup(&client->ctx);
  if (client->ctx.arena) {
    csilk_arena_free(client->ctx.arena);
  }
  free(client->current_header_field);
  free(client->current_header_value);
  free(client->current_url);
  csilk_server_t* srv = client->server;
  pool_put(srv, client);
}

/** @brief Close handler for client connections.
 * @param handle UV handle associated with client connection.
 */
static void on_close(uv_handle_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (client) {
    client->ctx._internal_client = NULL;
    uv_timer_stop(&client->timer);
    uv_timer_stop(&client->read_timer);
    uv_timer_stop(&client->write_timer);
    uv_timer_stop(&client->request_timer);

    client->close_pending = 4;
    uv_handle_t* timers[] = {(uv_handle_t*)&client->timer,
                             (uv_handle_t*)&client->read_timer,
                             (uv_handle_t*)&client->write_timer,
                             (uv_handle_t*)&client->request_timer};
    for (int i = 0; i < 3; i++) {
      if (uv_is_closing(timers[i])) {
        client->close_pending--;
      } else {
        timers[i]->data = client;
        uv_close(timers[i], on_timer_close);
      }
    }
    if (client->close_pending <= 0) {
      csilk_server_t* srv = client->server;
      if (srv) atomic_fetch_sub(&srv->active_connections, 1);
      csilk_ctx_cleanup(&client->ctx);
      if (client->ctx.arena) {
        csilk_arena_free(client->ctx.arena);
      }
      free(client->current_header_field);
      free(client->current_header_value);
      free(client->current_url);
      pool_put(srv, client);
    }
  }
}

/** @brief Signal handler for graceful shutdown.
 * @param handle Signal handle.
 * @param signum Signal number. */
static void on_signal(uv_signal_t* handle, int signum) {
  (void)signum;
  csilk_server_t* server = (csilk_server_t*)handle->data;
  csilk_server_stop(server);
}

/** @brief Async callback to stop the server gracefully.
 * Closes server-level handles and lets client connections drain naturally.
 * @param handle Async handle. */
static void on_stop_async(uv_async_t* handle) {
  csilk_server_t* server = (csilk_server_t*)handle->data;

  // Close the server listen handle (stop accepting new connections)
  if (!uv_is_closing((uv_handle_t*)&server->server_handle)) {
    uv_close((uv_handle_t*)&server->server_handle, on_server_handle_close);
  }

  // Close signal and async handles
  if (uv_is_active((uv_handle_t*)&server->sig_handle) &&
      !uv_is_closing((uv_handle_t*)&server->sig_handle)) {
    uv_close((uv_handle_t*)&server->sig_handle, on_server_handle_close);
  }
  if (uv_is_active((uv_handle_t*)&server->async_handle) &&
      !uv_is_closing((uv_handle_t*)&server->async_handle)) {
    uv_close((uv_handle_t*)&server->async_handle, on_server_handle_close);
  }
}

/** @brief Write completion callback.
 * @param req Write request.
 * @param status Write status. */
static void on_write(uv_write_t* req, int status) {
  if (status < 0) {
    fprintf(stderr, "Write error %s\n", uv_strerror(status));
  }
  // Stop write timeout (response flushed)
  if (req->handle) {
    csilk_client_t* client = (csilk_client_t*)req->handle->data;
    if (client) {
      uv_timer_stop(&client->write_timer);
    }
  }
  if (req->data) {
    free(req->data);
  }
  free(req);
}

/** @brief Idle timeout callback — closes connection on keep-alive idle.
 * @param handle Timer handle. */
static void on_idle_timeout(uv_timer_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (!uv_is_closing((uv_handle_t*)&client->handle)) {
    uv_close((uv_handle_t*)&client->handle, on_close);
  }
}

/** @brief Read timeout callback — no data received within read_timeout_ms.
 * @param handle Timer handle. */
static void on_read_timeout(uv_timer_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (!uv_is_closing((uv_handle_t*)&client->handle)) {
    uv_close((uv_handle_t*)&client->handle, on_close);
  }
}

/** @brief Write timeout callback — response not flushed within
 * write_timeout_ms.
 * @param handle Timer handle. */
static void on_write_timeout(uv_timer_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (!uv_is_closing((uv_handle_t*)&client->handle)) {
    uv_close((uv_handle_t*)&client->handle, on_close);
  }
}

/** @brief HTTP parser callback: message begins.
 * @param p HTTP parser instance. */
static int on_message_begin(llhttp_t* p) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  client->total_header_size = 0;
  client->header_count = 0;

  // Restart request timeout for this new request (keep-alive)
  if (client->server->config.request_timeout_ms > 0) {
    uv_timer_stop(&client->request_timer);
    uv_timer_start(&client->request_timer, on_read_timeout,
                   client->server->config.request_timeout_ms, 0);
  }
  return 0;
}

/** @brief HTTP parser callback: URL received.
 * @param p HTTP parser instance.
 * @param at Pointer to URL data.
 * @param length Length of URL data. */
static int on_url(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  size_t max_url = client->server->config.max_url_size;
  if (max_url > 0 && length > max_url) {
    return HPE_USER;
  }
  if (client->current_url) free(client->current_url);
  client->current_url = malloc(length + 1);
  if (!client->current_url) {
    return HPE_USER;
  }
  memcpy(client->current_url, at, length);
  client->current_url[length] = '\0';
  return 0;
}

/** @brief HTTP parser callback: header field name received.
 * @param p HTTP parser instance.
 * @param at Pointer to header field data.
 * @param length Length of header field data. */
static int on_header_field(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  client->total_header_size += length;
  if (client->total_header_size > client->server->config.max_header_size) {
    return HPE_USER;
  }
  client->header_count++;
  if (client->server->config.max_headers_count > 0 &&
      client->header_count > client->server->config.max_headers_count) {
    return HPE_USER;
  }

  if (client->current_header_field && client->current_header_value) {
    csilk_set_request_header(&client->ctx, client->current_header_field,
                             client->current_header_value);
    free(client->current_header_field);
    client->current_header_field = NULL;
    client->header_field_capacity = 0;
    free(client->current_header_value);
    client->current_header_value = NULL;
    client->header_value_capacity = 0;
  } else if (client->current_header_field) {
    free(client->current_header_field);
    client->current_header_field = NULL;
    client->header_field_capacity = 0;
  }

  client->current_header_field = malloc(length + 1);
  if (!client->current_header_field) {
    return HPE_USER;
  }
  memcpy(client->current_header_field, at, length);
  client->current_header_field[length] = '\0';
  return 0;
}

/** @brief HTTP parser callback: header value received.
 * @param p HTTP parser instance.
 * @param at Pointer to header value data.
 * @param length Length of header value data. */
/** @brief Grow a buffer to at least `needed` bytes using exponential doubling.
 */
static char* buf_grow(char* buf, size_t* cap, size_t needed) {
  if (needed <= *cap) return buf;
  size_t new_cap = *cap ? *cap : 32;
  while (new_cap < needed) new_cap *= 2;
  char* new_buf = realloc(buf, new_cap);
  if (!new_buf) return NULL;
  *cap = new_cap;
  return new_buf;
}

static int on_header_value(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  client->total_header_size += length;
  if (client->total_header_size > client->server->config.max_header_size) {
    return HPE_USER;
  }

  size_t prev_len =
      client->current_header_value ? strlen(client->current_header_value) : 0;
  size_t needed = prev_len + length + 1;
  char* new_val = buf_grow(client->current_header_value,
                           &client->header_value_capacity, needed);
  if (!new_val) {
    free(client->current_header_value);
    client->current_header_value = NULL;
    client->header_value_capacity = 0;
    client->total_header_size = 0;
    return HPE_USER;
  }
  client->current_header_value = new_val;
  memcpy(client->current_header_value + prev_len, at, length);
  client->current_header_value[prev_len + length] = '\0';
  return 0;
}

/** @brief HTTP parser callback: all headers received.
 * @param p HTTP parser instance. */
static int on_headers_complete(llhttp_t* p) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  if (client->current_header_field && client->current_header_value) {
    csilk_set_request_header(&client->ctx, client->current_header_field,
                             client->current_header_value);
    free(client->current_header_field);
    client->current_header_field = NULL;
    client->header_field_capacity = 0;
    free(client->current_header_value);
    client->current_header_value = NULL;
    client->header_value_capacity = 0;
  }
  return 0;
}

/** @brief HTTP parser callback: body data received.
 * @param p HTTP parser instance.
 * @param at Pointer to body data.
 * @param length Length of body data. */
static int on_body(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  if (client->ctx.request.body_len + length >
      client->server->config.max_body_size) {
    return HPE_USER;
  }
  char* new_body = realloc(client->ctx.request.body,
                           client->ctx.request.body_len + length + 1);
  if (new_body) {
    memcpy(new_body + client->ctx.request.body_len, at, length);
    client->ctx.request.body_len += length;
    new_body[client->ctx.request.body_len] = '\0';
    client->ctx.request.body = new_body;
  } else {
    free(client->ctx.request.body);
    client->ctx.request.body = NULL;
    client->ctx.request.body_len = 0;
    return HPE_USER;
  }
  return 0;
}

/** @brief Map HTTP status code to reason phrase. */
static const char* get_status_text(int status) {
  switch (status) {
    case CSILK_STATUS_SWITCHING_PROTOCOLS:
      return "Switching Protocols";
    case CSILK_STATUS_OK:
      return "OK";
    case CSILK_STATUS_CREATED:
      return "Created";
    case CSILK_STATUS_NO_CONTENT:
      return "No Content";
    case CSILK_STATUS_BAD_REQUEST:
      return "Bad Request";
    case CSILK_STATUS_UNAUTHORIZED:
      return "Unauthorized";
    case CSILK_STATUS_FORBIDDEN:
      return "Forbidden";
    case CSILK_STATUS_NOT_FOUND:
      return "Not Found";
    case CSILK_STATUS_INTERNAL_SERVER_ERROR:
      return "Internal Server Error";
    default:
      return "OK";
  }
}

/** @brief Send the fully constructed HTTP response to the client. */
void _csilk_send_response(csilk_ctx_t* c) {
  csilk_client_t* client = (csilk_client_t*)c->_internal_client;
  if (!client) return;

  // Stop request timeout timer (response is being sent)
  uv_timer_stop(&client->request_timer);

  int status = client->ctx.response.status ? client->ctx.response.status : 200;
  const char* status_text = get_status_text(status);

  // If response has body, use Content-Length
  int use_chunked =
      (client->ctx.response.body_len == 0 && client->ctx.is_async);
  const char* transfer_encoding =
      use_chunked ? "Transfer-Encoding: chunked\r\n" : "";

  size_t custom_headers_len = 0;
  for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
    for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h;
         h = h->next) {
      custom_headers_len += h->key_len + 2 + h->value_len + 2;
    }
  }

  size_t body_len = client->ctx.response.body_len;
  const char* body = client->ctx.response.body ? client->ctx.response.body : "";

  int keep_alive = llhttp_should_keep_alive(&client->parser);
  const char* connection_val = keep_alive ? "keep-alive" : "close";

  int header_len;
  if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
    header_len = snprintf(NULL, 0, "HTTP/1.1 101 Switching Protocols\r\n");
  } else if (use_chunked) {
    header_len =
        snprintf(NULL, 0,
                 "HTTP/1.1 %d %s\r\n"
                 "%s"
                 "Connection: %s\r\n",
                 status, status_text, transfer_encoding, connection_val);
  } else {
    header_len = snprintf(NULL, 0,
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n",
                          status, status_text, body_len, connection_val);
  }

  if (header_len < 0) return;

  // For non-chunked response, the length should be header + headers + crlf +
  // body
  size_t response_len = (size_t)header_len + custom_headers_len + 2 +
                        (use_chunked ? 0 : body_len);

  uv_write_t* req = malloc(sizeof(uv_write_t));
  if (req) {
    char* write_base = malloc(response_len + 1);
    if (write_base) {
      int pos;
      if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
        pos = snprintf(write_base, response_len + 1,
                       "HTTP/1.1 101 Switching Protocols\r\n");
      } else if (use_chunked) {
        pos = snprintf(write_base, response_len + 1,
                       "HTTP/1.1 %d %s\r\n"
                       "%s"
                       "Connection: %s\r\n",
                       status, status_text, transfer_encoding, connection_val);
      } else {
        pos = snprintf(write_base, response_len + 1,
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: %s\r\n",
                       status, status_text, body_len, connection_val);
      }

      for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h;
             h = h->next) {
          memcpy(write_base + pos, h->key, h->key_len);
          pos += (int)h->key_len;
          write_base[pos++] = ':';
          write_base[pos++] = ' ';
          memcpy(write_base + pos, h->value, h->value_len);
          pos += (int)h->value_len;
          write_base[pos++] = '\r';
          write_base[pos++] = '\n';
        }
      }

      if (!use_chunked) {
        snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n%s",
                 body);
      } else {
        snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n");
      }

      uv_buf_t buf = uv_buf_init(
          write_base, (use_chunked ? (size_t)pos + 2 : response_len));
      req->data = write_base;
      uv_write(req, (uv_stream_t*)&client->handle, &buf, 1, on_write);
    } else {
      free(req);
    }
  }

  // Stop read timeout (request is complete)
  uv_timer_stop(&client->read_timer);

  // Start write timeout
  if (client->server->config.write_timeout_ms > 0) {
    uv_timer_start(&client->write_timer, on_write_timeout,
                   client->server->config.write_timeout_ms, 0);
  }

  if (client->ctx.is_websocket) {
    // WebSocket or SSE connection: keep alive without idle timer
  } else {
    if (keep_alive) {
      uv_timer_start(&client->timer, on_idle_timeout,
                     client->server->config.idle_timeout_ms, 0);
      uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
    } else {
      if (!uv_is_closing((uv_handle_t*)&client->handle)) {
        uv_close((uv_handle_t*)&client->handle, on_close);
      }
    }
  }

  csilk_ctx_cleanup(&client->ctx);
}

/** @brief Finalize request headers and URL before routing. */
static void finalize_request(csilk_client_t* client, llhttp_t* p) {
  if (client->current_header_field && client->current_header_value) {
    csilk_set_request_header(&client->ctx, client->current_header_field,
                             client->current_header_value);
    free(client->current_header_field);
    client->current_header_field = NULL;
    client->header_field_capacity = 0;
    free(client->current_header_value);
    client->current_header_value = NULL;
    client->header_value_capacity = 0;
  }

  if (client->current_url) {
    char* path = NULL;
    char* query = NULL;
    csilk_split_url(client->current_url, &path, &query);
    if (client->ctx.request.path) {
      free((void*)client->ctx.request.path);
    }
    client->ctx.request.path = path;
    if (query) {
      csilk_parse_query(&client->ctx, query);
      free(query);
    }
    free(client->current_url);
    client->current_url = NULL;
  }

  client->ctx.request.method = (char*)llhttp_method_name(llhttp_get_method(p));
}

/** @brief HTTP parser callback: full request message parsed.
 * Routes the request to matching handlers and sends response.
 * @param p HTTP parser instance. */
static int on_message_complete(llhttp_t* p) {
  csilk_client_t* client = (csilk_client_t*)p->data;

  finalize_request(client, p);
  CSILK_LOG_I("Request: %s %s", client->ctx.request.method,
              client->ctx.request.path);

  if (csilk_router_match_ctx(client->server->router, &client->ctx)) {
    CSILK_LOG_D("Route matched, calling next handler");

    // Prepend global middlewares
    if (client->server->middleware_count > 0) {
      int route_handler_count = 0;
      while (client->ctx.handlers[route_handler_count] != NULL) {
        route_handler_count++;
      }

      int total_count = client->server->middleware_count + route_handler_count;
      csilk_handler_t* arena_handlers = csilk_arena_alloc(
          client->ctx.arena, (total_count + 1) * sizeof(csilk_handler_t));
      if (arena_handlers) {
        for (int i = 0; i < client->server->middleware_count; i++) {
          arena_handlers[i] = client->server->middlewares[i];
        }
        for (int i = 0; i < route_handler_count; i++) {
          arena_handlers[client->server->middleware_count + i] =
              client->ctx.handlers[i];
        }
        arena_handlers[total_count] = NULL;
        client->ctx.handlers = arena_handlers;
      }
    }

    csilk_next(&client->ctx);
  } else {
    CSILK_LOG_W("Route not found: %s", client->ctx.request.path);
    if (client->server->not_found_handler) {
      client->server->not_found_handler(&client->ctx);
    } else {
      csilk_string(&client->ctx, CSILK_STATUS_NOT_FOUND, "Not Found");
    }
  }

  if (client->ctx.is_async) {
    uv_read_stop((uv_stream_t*)&client->handle);
  }

  if (!client->ctx.is_async) {
    _csilk_send_response(&client->ctx);
  }
  return 0;
}

/** @brief libuv connection callback: accept new TCP connection.
 * Creates a client context, initializes parser, timer, and starts reading.
 * @param server_stream UV stream handle for the server.
 * @param status Connection status. */
static void on_new_connection(uv_stream_t* server_stream, int status) {
  if (status < 0) {
    fprintf(stderr, "New connection error %s\n", uv_strerror(status));
    return;
  }

  csilk_server_t* server = (csilk_server_t*)server_stream->data;

  int max_conn = server->config.max_connections;
  if (max_conn == 0) max_conn = server->max_connections;
  if (max_conn > 0 && atomic_load(&server->active_connections) >= max_conn) {
    /* accept and immediately close to drain the backlog */
    uv_tcp_t tmp;
    uv_tcp_init(server_stream->loop, &tmp);
    if (uv_accept(server_stream, (uv_stream_t*)&tmp) == 0)
      uv_close((uv_handle_t*)&tmp, NULL);
    return;
  }

  csilk_client_t* client = pool_get(server);
  if (!client) return;

  client->server = server;
  int r = uv_tcp_init(server_stream->loop, &client->handle);
  if (r < 0) {
    fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
    pool_put(server, client);
    return;
  }
  client->handle.data = client;
  client->ctx._internal_client = client;

  if (uv_accept(server_stream, (uv_stream_t*)&client->handle) == 0) {
    if (server->config.tcp_nodelay) {
      uv_tcp_nodelay((uv_tcp_t*)&client->handle, 1);
    }
    atomic_fetch_add(&server->active_connections, 1);
    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    client->parser.data = client;
    uv_timer_init(server_stream->loop, &client->timer);
    client->timer.data = client;
    uv_timer_init(server_stream->loop, &client->read_timer);
    client->read_timer.data = client;
    uv_timer_init(server_stream->loop, &client->write_timer);
    client->write_timer.data = client;
    uv_timer_init(server_stream->loop, &client->request_timer);
    client->request_timer.data = client;

    // Start read timeout and request timeout timers
    if (server->config.read_timeout_ms > 0) {
      uv_timer_start(&client->read_timer, on_read_timeout,
                     server->config.read_timeout_ms, 0);
    }
    if (server->config.request_timeout_ms > 0) {
      uv_timer_start(&client->request_timer, on_read_timeout,
                     server->config.request_timeout_ms, 0);
    }

    // Initialize arena for the request
    client->ctx.arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);

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

/** @brief Read callback.
 * @param stream Stream.
 * @param nread Number of bytes read.
 * @param buf Buffer read into. */
static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  csilk_client_t* client = (csilk_client_t*)stream->data;
  uv_timer_stop(&client->timer);
  // Reset read timeout on data arrival
  if (client->server->config.read_timeout_ms > 0) {
    uv_timer_start(&client->read_timer, on_read_timeout,
                   client->server->config.read_timeout_ms, 0);
  }
  if (nread > 0) {
    if (client->ctx.is_websocket) {
      csilk_ws_parse_frame(&client->ctx, (const uint8_t*)buf->base,
                           (size_t)nread);
    } else {
      enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);
      if (err == HPE_CLOSED_CONNECTION) {
        llhttp_init(&client->parser, HTTP_REQUEST, &client->server->settings);
        client->parser.data = client;
      } else if (err != HPE_OK) {
        fprintf(stderr, "Parse error: %s %s\n", llhttp_errno_name(err),
                client->parser.reason);
        if (!uv_is_closing((uv_handle_t*)stream)) {
          uv_close((uv_handle_t*)stream, on_close);
        }
      }
    }
  } else if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "Read error %s\n", uv_err_name((int)nread));
    }
    if (!uv_is_closing((uv_handle_t*)stream)) {
      uv_close((uv_handle_t*)stream, on_close);
    }
  }

  if (buf->base) {
    free(buf->base);
  }
}

/** @brief Get the client's IP address from the underlying connection. */
const char* csilk_get_client_ip(csilk_ctx_t* c) {
  if (!c || !c->_internal_client) return NULL;
  csilk_client_t* client = (csilk_client_t*)c->_internal_client;
  struct sockaddr_storage addr;
  int len = sizeof(addr);
  if (uv_tcp_getpeername(&client->handle, (struct sockaddr*)&addr, &len) == 0) {
    char ip[46];
    if (addr.ss_family == AF_INET) {
      uv_ip4_name((struct sockaddr_in*)&addr, ip, sizeof(ip));
    } else {
      uv_ip6_name((struct sockaddr_in6*)&addr, ip, sizeof(ip));
    }
    return csilk_arena_strdup(c->arena, ip);
  }
  return NULL;
}

#include "csilk_reflect.h"

/** @brief Create a new server instance. */
csilk_server_t* csilk_server_new(csilk_router_t* router) {
  csilk_reflect_init();
  csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
  if (!s) return NULL;
  s->loop = uv_default_loop();
  if (!s->loop) {
    free(s);
    return NULL;
  }
  s->router = router;
  llhttp_settings_init(&s->settings);
  s->settings.on_message_begin = on_message_begin;
  s->settings.on_url = on_url;
  s->settings.on_header_field = on_header_field;
  s->settings.on_header_value = on_header_value;
  s->settings.on_headers_complete = on_headers_complete;
  s->settings.on_body = on_body;
  s->settings.on_message_complete = on_message_complete;

  s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
  s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
  s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
  s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;

  return s;
}

/** @brief Built-in SPA fallback handler — serves index.html for unmatched
 * GET requests. */
static void spa_fallback_handler(csilk_ctx_t* c) {
  const char* method = csilk_get_method(c);
  if (!method || strcmp(method, "GET") != 0) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }
  csilk_client_t* client = (csilk_client_t*)c->_internal_client;
  if (!client || !client->server || !client->server->spa_doc_root) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/index.html", client->server->spa_doc_root);

  FILE* f = fopen(path, "rb");
  if (!f) {
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  if (fsize <= 0) {
    fclose(f);
    csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
    return;
  }
  rewind(f);
  char* body = malloc((size_t)fsize + 1);
  if (!body) {
    fclose(f);
    csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
    return;
  }
  size_t nread = fread(body, 1, (size_t)fsize, f);
  fclose(f);
  body[nread] = '\0';

  csilk_set_header(c, "Content-Type", "text/html");
  c->response.body = body;
  c->response.body_len = nread;
  c->response.body_is_managed = 1;
  csilk_status(c, CSILK_STATUS_OK);
}

/** @brief Set a custom handler for unmatched routes (404). */
void csilk_server_set_not_found_handler(csilk_server_t* server,
                                        csilk_handler_t handler) {
  if (!server) return;
  server->not_found_handler = handler;
}

/** @brief Enable SPA fallback: unmatched GET requests serve index.html.
 * Overrides any custom 404 handler.
 * @param server Server instance.
 * @param doc_root Directory containing index.html. */
void csilk_server_set_spa_fallback(csilk_server_t* server,
                                   const char* doc_root) {
  if (!server || !doc_root) return;
  free(server->spa_doc_root);
  server->spa_doc_root = strdup(doc_root);
  if (server->spa_doc_root) server->not_found_handler = spa_fallback_handler;
}

/** @brief Add a global middleware handler to the server. */
int csilk_server_use(csilk_server_t* server, csilk_handler_t handler) {
  if (!server || !handler) return -1;
  if (server->middleware_count >= 32) {
    CSILK_LOG_E("Global middleware limit (32) reached. Middleware dropped.");
    return -1;
  }
  server->middlewares[server->middleware_count++] = handler;
  return 0;
}

/** @brief Callback for closing server-level handles — no-op. */
static void on_server_handle_close(uv_handle_t* handle) { (void)handle; }

/** @brief Free server resources. Should be called after the loop has stopped.
 */
void csilk_server_free(csilk_server_t* server) {
  if (!server) return;

  // Join worker threads (they will exit when their loops stop)
  if (server->worker_tids) {
    for (int i = 0; i < server->worker_count; i++) {
      uv_thread_join(&server->worker_tids[i]);
    }
    free(server->worker_tids);
    server->worker_tids = NULL;
  }

  free(server->spa_doc_root);
  for (int i = 0; i < server->client_pool_count; i++) {
    free(server->client_pool[i]);
  }
  free(server);
}

/** @brief Signal the server to stop gracefully. */
void csilk_server_stop(csilk_server_t* server) {
  if (!server) return;
  uv_async_send(&server->async_handle);
}

/** @brief Apply server configuration. */
void csilk_server_set_config(csilk_server_t* server,
                             const csilk_server_config_t* config) {
  if (!server || !config) return;
  server->config = *config;
}

/** @brief Set the maximum number of concurrent client connections. */
int csilk_server_set_max_connections(csilk_server_t* server, int max) {
  if (!server) return -1;
  int prev = server->max_connections;
  server->max_connections = max;
  return prev;
}

#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#endif

static int bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port,
                           int backlog, bool reuseport);

/** @brief Worker thread initialization data for multi-loop mode. */
typedef struct {
  csilk_server_t* server; /**< Server instance. */
  int port;               /**< Port to listen on. */
} worker_data_t;

/** @brief Worker thread entry point for multi-loop SO_REUSEPORT mode.
 * Each worker runs its own libuv event loop and accept loop.
 * @param arg Pointer to worker_data_t (freed inside). */
static void worker_thread(void* arg) {
  worker_data_t* data = (worker_data_t*)arg;
  csilk_server_t* server = data->server;
  int port = data->port;
  free(data);

  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_tcp_t server_handle;
  server_handle.data = server;

  if (bind_and_listen(&loop, &server_handle, port,
                      server->config.listen_backlog, true) < 0) {
    uv_loop_close(&loop);
    return;
  }

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}

// UV_HANDLE_BOUND lives in libuv's private uv-common.h; define it here
// so we can set it after uv_tcp_open for the SO_REUSEPORT path.
#ifndef UV_HANDLE_BOUND
#define UV_HANDLE_BOUND 0x00002000
#endif

/** @brief Create, bind, and listen a TCP socket with SO_REUSEPORT support.
 * On non-Windows with reuseport=true, creates socket manually so
 * SO_REUSEPORT is set before bind (required by kernel).
 * @param loop Event loop to attach to.
 * @param out_handle [out] Initialized TCP handle (must not be freed by caller).
 * @param port Port number.
 * @param backlog Listen backlog depth.
 * @param reuseport Enable SO_REUSEPORT for multi-worker sharing.
 * @return 0 on success, -1 on error. */
static int bind_and_listen(uv_loop_t* loop, uv_tcp_t* out_handle, int port,
                           int backlog, bool reuseport) {
#ifndef _WIN32
  if (reuseport) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", port, &addr);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      close(fd);
      return -1;
    }
    if (listen(fd, backlog) < 0) {
      close(fd);
      return -1;
    }
    int r = uv_tcp_init(loop, out_handle);
    if (r < 0) {
      close(fd);
      return -1;
    }
    r = uv_tcp_open(out_handle, (uv_os_sock_t)fd);
    if (r < 0) {
      close(fd);
      return -1;
    }
    // uv_tcp_open does not set UV_HANDLE_BOUND; uv_listen requires
    // it, so set it manually before calling uv_listen.
    out_handle->flags |= UV_HANDLE_BOUND;
    return uv_listen((uv_stream_t*)out_handle, backlog, on_new_connection);
  }
#endif
  int r = uv_tcp_init(loop, out_handle);
  if (r < 0) return -1;
  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", port, &addr);
  r = uv_tcp_bind(out_handle, (const struct sockaddr*)&addr, 0);
  if (r < 0) return -1;
  return uv_listen((uv_stream_t*)out_handle, backlog, on_new_connection);
}

/** @brief Run the server event loop.
 * Blocks until server is stopped or error occurs. */
int csilk_server_run(csilk_server_t* server, int port) {
  if (!server) return -1;

  int workers = server->config.worker_threads;
  if (workers <= 0) workers = 1;

  // Initialize async handle for thread-safe stop
  int r = uv_async_init(server->loop, &server->async_handle, on_stop_async);
  if (r < 0) return -1;
  server->async_handle.data = server;

  r = bind_and_listen(server->loop, &server->server_handle, port,
                      server->config.listen_backlog, workers > 1);
  if (r < 0) return -1;
  server->server_handle.data = server;

  if (server->config.tcp_keepalive > 0) {
    uv_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
  }

  if (workers > 1) {
    server->worker_tids = malloc((size_t)(workers - 1) * sizeof(uv_thread_t));
    if (server->worker_tids) {
      server->worker_count = workers - 1;
      for (int i = 0; i < workers - 1; i++) {
        worker_data_t* data = malloc(sizeof(worker_data_t));
        if (!data) continue;
        data->server = server;
        data->port = port;
        uv_thread_create(&server->worker_tids[i], worker_thread, data);
      }
    }
  }

  r = uv_signal_init(server->loop, &server->sig_handle);
  if (r < 0) {
    uv_close((uv_handle_t*)&server->async_handle, NULL);
    uv_close((uv_handle_t*)&server->server_handle, NULL);
    return -1;
  }
  server->sig_handle.data = server;
  r = uv_signal_start(&server->sig_handle, on_signal, SIGINT);
  if (r < 0) {
    uv_close((uv_handle_t*)&server->sig_handle, NULL);
    uv_close((uv_handle_t*)&server->async_handle, NULL);
    uv_close((uv_handle_t*)&server->server_handle, NULL);
    return -1;
  }

  CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port,
              workers);

  return uv_run(server->loop, UV_RUN_DEFAULT);
}
