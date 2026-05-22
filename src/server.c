#include "gin.h"
#include <uv.h>
#include <llhttp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct gin_server_s {
    uv_loop_t *loop;
    gin_router_t *router;
    uv_tcp_t server_handle;
    llhttp_settings_t settings;
};

typedef struct {
    uv_tcp_t handle;
    llhttp_t parser;
    gin_server_t *server;
} gin_client_t;

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    if (buf->base == NULL) {
        buf->len = 0;
    } else {
        buf->len = suggested_size;
    }
}

static void on_close(uv_handle_t *handle) {
    gin_client_t *client = (gin_client_t *)handle->data;
    free(client);
}

static void on_write(uv_write_t *req, int status) {
    if (status) {
        fprintf(stderr, "Write error %s\n", uv_strerror(status));
    }
    free(req->data);
    free(req);
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    gin_client_t *client = (gin_client_t *)stream->data;

    if (nread > 0) {
        // Echo back for MVP
        uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
        if (req != NULL) {
            char *write_base = malloc(nread);
            if (write_base != NULL) {
                uv_buf_t write_buf = uv_buf_init(write_base, nread);
                memcpy(write_base, buf->base, nread);
                req->data = write_base;
                int r = uv_write(req, stream, &write_buf, 1, on_write);
                if (r < 0) {
                    fprintf(stderr, "uv_write error %s\n", uv_strerror(r));
                    free(write_base);
                    free(req);
                }
            } else {
                free(req);
            }
        }

        // Feed into llhttp
        enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);
        if (err != HPE_OK) {
            fprintf(stderr, "Parse error: %s %s\n", llhttp_errno_name(err), client->parser.reason);
            if (!uv_is_closing((uv_handle_t *)stream)) {
                uv_close((uv_handle_t *)stream, on_close);
            }
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
        }
        if (!uv_is_closing((uv_handle_t *)stream)) {
            uv_close((uv_handle_t *)stream, on_close);
        }
    }

    if (buf->base) {
        free(buf->base);
    }
}

static void on_new_connection(uv_stream_t *server_stream, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }

    gin_server_t *server = (gin_server_t *)server_stream->data;
    gin_client_t *client = malloc(sizeof(gin_client_t));
    if (!client) return;

    client->server = server;
    int r = uv_tcp_init(server->loop, &client->handle);
    if (r < 0) {
        fprintf(stderr, "uv_tcp_init error %s\n", uv_strerror(r));
        free(client);
        return;
    }
    client->handle.data = client;

    if (uv_accept(server_stream, (uv_stream_t *)&client->handle) == 0) {
        llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);

        r = uv_read_start((uv_stream_t *)&client->handle, alloc_buffer, on_read);
        if (r < 0) {
            fprintf(stderr, "uv_read_start error %s\n", uv_strerror(r));
            if (!uv_is_closing((uv_handle_t *)&client->handle)) {
                uv_close((uv_handle_t *)&client->handle, on_close);
            }
        }
    } else {
        if (!uv_is_closing((uv_handle_t *)&client->handle)) {
            uv_close((uv_handle_t *)&client->handle, on_close);
        }
    }
}

gin_server_t* gin_server_new(gin_router_t *router) {
    gin_server_t *s = calloc(1, sizeof(gin_server_t));
    if (!s) return NULL;
    s->loop = uv_default_loop();
    if (!s->loop) {
        free(s);
        return NULL;
    }
    s->router = router;
    llhttp_settings_init(&s->settings);
    return s;
}

void gin_server_free(gin_server_t *server) {
    if (server) {
        if (server->server_handle.loop && !uv_is_closing((uv_handle_t *)&server->server_handle)) {
            uv_close((uv_handle_t *)&server->server_handle, NULL);
        }
        free(server);
    }
}

int gin_server_run(gin_server_t *server, int port) {
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

    r = uv_tcp_bind(&server->server_handle, (const struct sockaddr *)&addr, 0);
    if (r < 0) {
        fprintf(stderr, "uv_tcp_bind error %s\n", uv_strerror(r));
        return r;
    }

    r = uv_listen((uv_stream_t *)&server->server_handle, 128, on_new_connection);
    if (r < 0) {
        fprintf(stderr, "uv_listen error %s\n", uv_strerror(r));
        return r;
    }

    return uv_run(server->loop, UV_RUN_DEFAULT);
}
