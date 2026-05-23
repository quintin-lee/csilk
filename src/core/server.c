/**
 * @file server.c
 * @brief Server implementation.
 * @copyright MIT License
 */

#include <llhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk.h"
#include "csilk_internal.h"

/** @brief Default idle timeout in milliseconds. */
#define CSILK_DEFAULT_IDLE_TIMEOUT  5000
/** @brief Default maximum request body size in bytes. */
#define CSILK_DEFAULT_MAX_BODY_SIZE (1024UL * 1024UL)
/** @brief Default maximum request header size in bytes. */
#define CSILK_DEFAULT_MAX_HEADER_SIZE (64UL * 1024UL)
/** @brief Default TCP listen backlog. */
#define CSILK_DEFAULT_LISTEN_BACKLOG 128
/** @brief Default request arena chunk size. */
#define CSILK_DEFAULT_ARENA_SIZE 4096

/** @brief Server structure. */
struct csilk_server_s {
  uv_loop_t* loop;              /**< libuv event loop. */
  csilk_router_t* router;         /**< Associated router instance. */
  uv_tcp_t server_handle;       /**< TCP server handle. */
  uv_signal_t sig_handle;        /**< SIGINT signal handler. */
  uv_async_t async_handle;      /**< Async handle for cross-thread wakeup. */
  llhttp_settings_t settings;   /**< HTTP parser callback settings. */
  csilk_server_config_t config;   /**< Server configuration. */
  csilk_handler_t middlewares[32]; /**< Global middlewares. */
  int middleware_count;          /**< Number of global middlewares. */
  int max_connections;           /**< Max concurrent connections (0=unlimited). */
  int active_connections;        /**< Current connection count. */
};

/** @brief Client connection structure. */
typedef struct {
  uv_tcp_t handle;                   /**< libuv TCP stream handle. */
  uv_timer_t timer;                  /**< Connection idle timer. */
  llhttp_t parser;                   /**< HTTP request parser. */
  csilk_server_t* server;              /**< Owning server instance. */
  csilk_ctx_t ctx;                     /**< Request context for this connection. */
  size_t total_header_size;          /**< Total size of headers parsed so far. */
  char* current_url;                 /**< Current URL being parsed. */
  char* current_header_field;        /**< Temporary header field name. */
  char* current_header_value;        /**< Temporary header field value. */
} csilk_client_t;

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

/** @brief Timer close callback.
 * @param handle Handle to close. */
static void on_timer_close(uv_handle_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (client) {
    if (client->server) client->server->active_connections--;
    client->ctx._internal_client = NULL;
    csilk_ctx_cleanup(&client->ctx);
    if (client->ctx.arena) {
      csilk_arena_free(client->ctx.arena);
      client->ctx.arena = NULL;
    }
    free(client->current_header_field);
    free(client->current_header_value);
    free(client->current_url);
    free(client);
  }
}

/** @brief Close handler for client connections.
 * @param handle UV handle associated with client connection.
 */
static void on_close(uv_handle_t* handle) {
  csilk_client_t* client = (csilk_client_t*)handle->data;
  if (client) {
    client->ctx._internal_client = NULL;
    uv_timer_stop(&client->timer);
    if (client->ctx.arena) {
        csilk_arena_free(client->ctx.arena);
        client->ctx.arena = NULL;
    }
    if (!uv_is_closing((uv_handle_t*)&client->timer)) {
      uv_close((uv_handle_t*)&client->timer, on_timer_close);
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

/** @brief Async callback to stop the server loop.
 * @param handle Async handle. */
static void on_stop_async(uv_async_t* handle) {
  csilk_server_t* server = (csilk_server_t*)handle->data;
  uv_stop(server->loop);
}

/** @brief Write completion callback.
 * @param req Write request.
 * @param status Write status. */
static void on_write(uv_write_t* req, int status) {
  if (status < 0) {
    fprintf(stderr, "Write error %s\n", uv_strerror(status));
  }
  if (req->data) {
    free(req->data);
  }
  free(req);
}

/** @brief Timeout callback.
 * @param handle Timer handle. */
static void on_timeout(uv_timer_t* handle) {
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
  // Reset other request-specific fields if necessary
  return 0;
}

/** @brief HTTP parser callback: URL received.
 * @param p HTTP parser instance.
 * @param at Pointer to URL data.
 * @param length Length of URL data. */
static int on_url(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
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

  if (client->current_header_field && client->current_header_value) {
    csilk_set_request_header(&client->ctx, client->current_header_field,
                            client->current_header_value);
    free(client->current_header_field);
    free(client->current_header_value);
    client->current_header_field = NULL;
    client->current_header_value = NULL;
  } else if (client->current_header_field) {
    // We had a field but no value yet? Should not happen in valid HTTP
    // but we free it to be safe.
    free(client->current_header_field);
    client->current_header_field = NULL;
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
static int on_header_value(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  client->total_header_size += length;
  if (client->total_header_size > client->server->config.max_header_size) {
      return HPE_USER;
  }

  size_t prev_len = client->current_header_value ? strlen(client->current_header_value) : 0;
  char* new_val = realloc(client->current_header_value, prev_len + length + 1);
  if (!new_val) {
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
    free(client->current_header_value);
    client->current_header_field = NULL;
    client->current_header_value = NULL;
  }
  return 0;
}

/** @brief HTTP parser callback: body data received.
 * @param p HTTP parser instance.
 * @param at Pointer to body data.
 * @param length Length of body data. */
static int on_body(llhttp_t* p, const char* at, size_t length) {
  csilk_client_t* client = (csilk_client_t*)p->data;
  if (client->ctx.request.body_len + length > client->server->config.max_body_size) {
      return HPE_USER;
  }
  char* new_body = realloc(client->ctx.request.body, client->ctx.request.body_len + length + 1);
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

/** @brief Send the fully constructed HTTP response to the client.
 * Builds the HTTP response from context status/headers/body, writes it
 * to the connection, then handles keep-alive or close.
 */
void _csilk_send_response(csilk_ctx_t* c) {
  csilk_client_t* client = (csilk_client_t*)c->_internal_client;
  if (!client) return;
  
  int status = client->ctx.response.status ? client->ctx.response.status : 200;
  const char* status_text;
  switch (status) {
    case 101:
      status_text = "Switching Protocols";
      break;
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

  size_t custom_headers_len = 0;
  for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
    for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h; h = h->next) {
      custom_headers_len += strlen(h->key) + 2 + strlen(h->value) + 2;
    }
  }

  size_t body_len = client->ctx.response.body_len;
  const char* body = client->ctx.response.body ? client->ctx.response.body : "";

  int keep_alive = llhttp_should_keep_alive(&client->parser);
  const char* connection_val = keep_alive ? "keep-alive" : "close";

  int header_len;
  if (status == 101) {
    header_len = snprintf(NULL, 0, "HTTP/1.1 101 Switching Protocols\r\n");
  } else {
    header_len = snprintf(NULL, 0,
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n",
                          status, status_text, body_len, connection_val);
  }

  if (header_len < 0) return;

  size_t response_len = (size_t)header_len + custom_headers_len + 2 + body_len;

  uv_write_t* req = malloc(sizeof(uv_write_t));
  if (req) {
    char* write_base = malloc(response_len + 1);
    if (write_base) {
      int pos;
      if (status == 101) {
        pos = snprintf(write_base, response_len + 1,
                       "HTTP/1.1 101 Switching Protocols\r\n");
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
          pos += snprintf(write_base + pos, response_len + 1 - (size_t)pos,
                          "%s: %s\r\n", h->key, h->value);
        }
      }

      snprintf(write_base + pos, response_len + 1 - (size_t)pos, "\r\n%s", body);

      uv_buf_t buf = uv_buf_init(write_base, response_len);
      req->data = write_base;
      uv_write(req, (uv_stream_t*)&client->handle, &buf, 1, on_write);
    } else {
      free(req);
    }
  }

  if (client->ctx.is_websocket) {
      // WebSocket or SSE connection: keep alive without idle timer
  } else {
      if (keep_alive) {
        uv_timer_start(&client->timer, on_timeout, client->server->config.idle_timeout_ms, 0);
        uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
      } else {
        if (!uv_is_closing((uv_handle_t*)&client->handle)) {
          uv_close((uv_handle_t*)&client->handle, on_close);
        }
      }
  }

  csilk_ctx_cleanup(&client->ctx);
}

/** @brief HTTP parser callback: full request message parsed.
 * Routes the request to matching handlers and sends response.
 * @param p HTTP parser instance. */
static int on_message_complete(llhttp_t* p) {
  csilk_client_t* client = (csilk_client_t*)p->data;

  if (client->current_header_field && client->current_header_value) {
    csilk_set_request_header(&client->ctx, client->current_header_field,
                            client->current_header_value);
    free(client->current_header_field);
    free(client->current_header_value);
    client->current_header_field = NULL;
    client->current_header_value = NULL;
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
    CSILK_LOG_I("Request: %s %s", client->ctx.request.method, client->ctx.request.path);

    if (csilk_router_match_ctx(client->server->router, &client->ctx)) {
        CSILK_LOG_D("Route matched, calling next handler");

        // Prepend global middlewares
        if (client->server->middleware_count > 0) {
            int route_handler_count = 0;
            while (client->ctx.handlers[route_handler_count] != NULL) {
                route_handler_count++;
            }
            
            int total_count = client->server->middleware_count + route_handler_count;
            csilk_handler_t* arena_handlers = csilk_arena_alloc(client->ctx.arena, (total_count + 1) * sizeof(csilk_handler_t));
            if (arena_handlers) {
                for (int i = 0; i < client->server->middleware_count; i++) {
                    arena_handlers[i] = client->server->middlewares[i];
                }
                for (int i = 0; i < route_handler_count; i++) {
                    arena_handlers[client->server->middleware_count + i] = client->ctx.handlers[i];
                }
                arena_handlers[total_count] = NULL;
                client->ctx.handlers = arena_handlers;
            }
        }

        csilk_next(&client->ctx);
    } else {
        CSILK_LOG_W("Route not found: %s", client->ctx.request.path);
        csilk_string(&client->ctx, 404, "Not Found");
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

  if (server->max_connections > 0 &&
      server->active_connections >= server->max_connections) {
    /* accept and immediately close to drain the backlog */
    uv_tcp_t tmp;
    uv_tcp_init(server_stream->loop, &tmp);
    if (uv_accept(server_stream, (uv_stream_t*)&tmp) == 0)
      uv_close((uv_handle_t*)&tmp, NULL);
    return;
  }

  csilk_client_t* client = calloc(1, sizeof(csilk_client_t));
  if (!client) return;

  client->server = server;
  int r = uv_tcp_init(server_stream->loop, &client->handle);
  if (r < 0) {
    fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
    free(client);
    return;
  }
  client->handle.data = client;
  client->ctx._internal_client = client;

  if (uv_accept(server_stream, (uv_stream_t*)&client->handle) == 0) {
    if (server->config.tcp_nodelay) {
      uv_tcp_nodelay((uv_tcp_t*)&client->handle, 1);
    }
    server->active_connections++;
    llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
    client->parser.data = client;
    uv_timer_init(server_stream->loop, &client->timer);
    client->timer.data = client;
    
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
  if (nread > 0) {
    if (client->ctx.is_websocket) {
        csilk_ws_parse_frame(&client->ctx, (const uint8_t*)buf->base, (size_t)nread);
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

/** @brief Create a new server instance. */
csilk_server_t* csilk_server_new(csilk_router_t* router) {
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

/** @brief Add a global middleware handler to the server. */
int csilk_server_use(csilk_server_t* server, csilk_handler_t handler) {
  if (!server || !handler) return -1;
  if (server->middleware_count >= 32) return -1;
  server->middlewares[server->middleware_count++] = handler;
  return 0;
}

/** @brief Deallocate server resources. */
void csilk_server_free(csilk_server_t* server) {
  if (!server) return;
  
  if (uv_is_active((uv_handle_t*)&server->server_handle)) {
      uv_close((uv_handle_t*)&server->server_handle, NULL);
  }
  if (uv_is_active((uv_handle_t*)&server->sig_handle)) {
      uv_close((uv_handle_t*)&server->sig_handle, NULL);
  }
  if (uv_is_active((uv_handle_t*)&server->async_handle)) {
      uv_close((uv_handle_t*)&server->async_handle, NULL);
  }
  
  // Note: Since uv_close is async, handles might not be fully closed yet.
  // But for a 'free' function that is typically called at the end of the process,
  // this is often the best we can do without running the loop until closed.
  
  free(server);
}

/** @brief Signal the server to stop gracefully. */
void csilk_server_stop(csilk_server_t* server) {
  if (!server) return;
  uv_async_send(&server->async_handle);
}

/** @brief Apply server configuration. */
void csilk_server_set_config(csilk_server_t* server, csilk_server_config_t config) {
  if (!server) return;
  server->config = config;
}

/** @brief Set the maximum number of concurrent client connections. */
int csilk_server_set_max_connections(csilk_server_t* server, int max) {
  if (!server) return -1;
  int prev = server->max_connections;
  server->max_connections = max;
  return prev;
}

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

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
  uv_tcp_init(&loop, &server_handle);
  server_handle.data = server;

#ifndef _WIN32
  int fd;
  if (uv_fileno((const uv_handle_t*)&server_handle, &fd) == 0) {
      int on = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
  }
#endif

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", port, &addr);

  int r = uv_tcp_bind(&server_handle, (const struct sockaddr*)&addr, 0);
  if (r < 0) {
    fprintf(stderr, "Worker bind error: %s\n", uv_strerror(r));
    return;
  }

  r = uv_listen((uv_stream_t*)&server_handle, server->config.listen_backlog,
                on_new_connection);
  if (r < 0) {
    fprintf(stderr, "Worker listen error: %s\n", uv_strerror(r));
    return;
  }

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}

/** @brief Run the server event loop.
 * Blocks until server is stopped or error occurs. */
int csilk_server_run(csilk_server_t* server, int port) {
  if (!server) return -1;

  int workers = server->config.worker_threads;
  if (workers <= 0) workers = 1;

  int r = uv_tcp_init(server->loop, &server->server_handle);
  if (r < 0) return -1;
  server->server_handle.data = server;

#ifndef _WIN32
  if (workers > 1) {
      int fd;
      if (uv_fileno((const uv_handle_t*)&server->server_handle, &fd) == 0) {
          int on = 1;
          setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
      }
  }
#endif

  // Initialize async handle for thread-safe stop
  r = uv_async_init(server->loop, &server->async_handle, on_stop_async);
  if (r < 0) {
    uv_close((uv_handle_t*)&server->server_handle, NULL);
    return -1;
  }
  server->async_handle.data = server;

  // Apply TCP settings
  if (server->config.tcp_keepalive > 0) {
      uv_tcp_keepalive(&server->server_handle, 1, server->config.tcp_keepalive);
  }

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", port, &addr);

  r = uv_tcp_bind(&server->server_handle, (const struct sockaddr*)&addr, 0);
  if (r < 0) return -1;

  r = uv_listen((uv_stream_t*)&server->server_handle, server->config.listen_backlog,
                on_new_connection);
  if (r < 0) return -1;

  if (workers > 1) {
    for (int i = 0; i < workers - 1; i++) {
      worker_data_t* data = malloc(sizeof(worker_data_t));
      data->server = server;
      data->port = port;
      uv_thread_t tid;
      uv_thread_create(&tid, worker_thread, data);
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

  printf("Server listening on port %d\n", port);
  fflush(stdout);

  return uv_run(server->loop, UV_RUN_DEFAULT);
}
