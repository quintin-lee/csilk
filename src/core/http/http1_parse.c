/**
 * @file http1_parse.c
 * @brief HTTP/1.1 request parsing: llhttp callbacks, header processing,
 *        body accumulation, and request dispatch.
 *
 * @copyright MIT License
 */

#include <assert.h>
#include <limits.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../primitives/header_map.h"
#include "csilk/core/http/h2.h"
#include "../internal/srv_impl.h"

/* --- Zero-copy header persistence --- */

/** @brief Persist a zero-copy header field+value pair into the request header map.
 *
 * Copies the header field and value from string views into the request arena
 * and inserts them into the request header hash map. This is the single point
 * where zero-copy references are materialized into persistent arena memory.
 *
 * @param c     Request context (for arena allocation).
 * @param field Header field name (zero-copy reference to recv buffer).
 * @param value Header value (zero-copy reference to recv buffer). */
void
_csilk_persist_header(csilk_ctx_t* c, const csilk_str_view_t* field, const csilk_str_view_t* value)
{
    if (!c || !c->arena || !field || !field->data || !value || !value->data) {
        return;
    }
    /* Use the zero-copy-aware map_set variant that copies from views. */
    map_set_view(c, &c->request.headers, field, value);
}

/* --- llHTTP parser callbacks --- */

/** @brief llhttp callback: HTTP message start.
 *
 * Called at the beginning of each HTTP request message (including
 * keep-alive connections). Resets header tracking counters and restarts
 * the request timeout timer.
 *
 * @param p The llhttp parser instance (data points to csilk_client_t).
 * @return 0 (HPE_OK) to continue parsing. */
int
on_message_begin(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->total_header_size = 0;
    client->header_count = 0;
    client->header_field_completed = 0;
    client->current_header_field.data = NULL;
    client->current_header_field.len = 0;
    client->current_header_value.data = NULL;
    client->current_header_value.len = 0;

    unsigned int req_timeout = _csilk_server_get_request_timeout_ms(client->server);
    if (req_timeout > 0) {
        csilk_io_timer_stop(&client->request_timer);
        csilk_io_timer_start(&client->request_timer, on_read_timeout, req_timeout, 0);
    }

    csilk_log_set_request_id(NULL);
    return 0;
}

/** @brief llhttp callback: URL data received.
 *
 * Stores a zero-copy reference to the URL in the receive buffer.
 * The URL pointer points directly into the receive buffer and is valid
 * until the request is fully processed. No heap allocation occurs.
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to the URL data.
 * @param length Length of the URL data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if URL exceeds max_url_size. */
int
on_url(llhttp_t* p, const char* at, size_t length)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    size_t          max_url = _csilk_server_get_max_url_size(client->server);
    if (max_url > 0 && length > max_url) {
        CSILK_LOG_W("URL length (%zu) exceeds max_url_size limit (%zu)", length, max_url);
        client->current_url.data = NULL;
        client->current_url.len = 0;
        return HPE_USER;
    }
    if (client->current_url.data && at == client->current_url.data + client->current_url.len) {
        /* URL continues — extend the reference. */
        client->current_url.len += length;
    } else if (client->current_url.data) {
        /* Split URL: must allocate and copy. */
        char* new_url = csilk_arena_alloc(client->ctx.arena, client->current_url.len + length + 1);
        if (!new_url) {
            client->current_url.data = NULL;
            client->current_url.len = 0;
            return HPE_USER;
        }
        memcpy(new_url, client->current_url.data, client->current_url.len);
        memcpy(new_url + client->current_url.len, at, length);
        client->current_url.data = new_url;
        client->current_url.len += length;
    } else {
        /* First chunk of URL */
        client->current_url.data = at;
        client->current_url.len = length;
    }
    return 0;
}

/** @brief llhttp callback: header field name received.
 *
 * Stores a zero-copy reference to the header field name in the receive
 * buffer. When a previous field+value pair is complete, stores it in
 * the request header map (via arena, single copy). Enforces
 * max_header_size and max_headers_count limits.
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to header field data.
 * @param length Length of the header field data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if size/count limits are exceeded. */
int
on_header_field(llhttp_t* p, const char* at, size_t length)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->total_header_size += length;
    size_t max_header = _csilk_server_get_max_header_size(client->server);
    if (client->total_header_size > max_header) {
        CSILK_LOG_W("Total header size limit exceeded on header field");
        return HPE_USER;
    }

    if (client->header_field_completed) {
        /* Previous field completed (e.g. empty value header without on_header_value) */
        if (client->current_header_field.data) {
            static const csilk_str_view_t empty_val = {.data = "", .len = 0};
            const csilk_str_view_t*       val =
                client->current_header_value.data ? &client->current_header_value : &empty_val;
            _csilk_persist_header(&client->ctx, &client->current_header_field, val);
            client->current_header_field.data = NULL;
            client->current_header_field.len = 0;
            client->current_header_value.data = NULL;
            client->current_header_value.len = 0;
        }
        client->header_field_completed = 0;
    }

    if (client->current_header_field.data) {
        /* Contiguous or split chunk continuation of the header field */
        if (at == client->current_header_field.data + client->current_header_field.len) {
            /* Contiguous chunk: zero-copy pointer+length extension (0 mallocs, 0 arena allocs) */
            client->current_header_field.len += length;
        } else {
            /* Non-contiguous chunk across buffers: concatenate in arena */
            char* new_field =
                csilk_arena_alloc(client->ctx.arena, client->current_header_field.len + length + 1);
            if (!new_field) {
                client->current_header_field.data = NULL;
                client->current_header_field.len = 0;
                return HPE_USER;
            }
            memcpy(new_field, client->current_header_field.data, client->current_header_field.len);
            memcpy(new_field + client->current_header_field.len, at, length);
            new_field[client->current_header_field.len + length] = '\0';
            client->current_header_field.data = new_field;
            client->current_header_field.len += length;
        }
    } else {
        /* First chunk of a new header field */
        client->header_count++;
        size_t max_count = _csilk_server_get_max_headers_count(client->server);
        if (max_count > 0 && client->header_count > max_count) {
            CSILK_LOG_W("Total header count limit exceeded (%zu)", client->header_count);
            return HPE_USER;
        }
        client->current_header_field.data = at;
        client->current_header_field.len = length;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
    }
    return 0;
}

/** @brief llhttp callback: header field name parsing completed (hit colon). */
int
on_header_field_complete(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->header_field_completed = 1;
    return 0;
}

/** @brief llhttp callback: header value data received.
 *
 * Accumulates a zero-copy reference to the header value. If a previous
 * header value exists (split across multiple buffers), the references are
 * merged into a single arena-allocated copy before updating the view.
 * This ensures each header is stored only once (in the arena hash map).
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to header value data.
 * @param length Length of header value data.
 * @return 0 (HPE_OK) on success, HPE_USER if size limit is exceeded. */
int
on_header_value(llhttp_t* p, const char* at, size_t length)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->total_header_size += length;
    size_t max_header = _csilk_server_get_max_header_size(client->server);
    if (client->total_header_size > max_header) {
        CSILK_LOG_W("Total header size limit exceeded on header value");
        client->current_header_field.data = NULL;
        client->current_header_field.len = 0;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
        return HPE_USER;
    }

    if (client->current_header_value.data) {
        /* Contiguous or split chunk continuation of the header value */
        if (at == client->current_header_value.data + client->current_header_value.len) {
            /* Contiguous chunk: zero-copy pointer+length extension (0 mallocs, 0 arena allocs) */
            client->current_header_value.len += length;
        } else {
            /* Non-contiguous chunk across buffers: concatenate in arena */
            char* new_val =
                csilk_arena_alloc(client->ctx.arena, client->current_header_value.len + length + 1);
            if (!new_val) {
                client->current_header_value.data = NULL;
                client->current_header_value.len = 0;
                return HPE_USER;
            }
            memcpy(new_val, client->current_header_value.data, client->current_header_value.len);
            memcpy(new_val + client->current_header_value.len, at, length);
            new_val[client->current_header_value.len + length] = '\0';
            client->current_header_value.data = new_val;
            client->current_header_value.len += length;
        }
    } else {
        /* First chunk of this value. */
        client->current_header_value.data = at;
        client->current_header_value.len = length;
    }
    return 0;
}

/** @brief llhttp callback: a single header (field+value) is complete.
 *
 * Persists the field+value pair into the request header map and clears the views.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) to continue parsing. */
int
on_header_value_complete(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->header_field_completed = 0;
    if (client->current_header_field.data) {
        static const csilk_str_view_t empty_val = {.data = "", .len = 0};
        const csilk_str_view_t*       val =
            client->current_header_value.data ? &client->current_header_value : &empty_val;
        _csilk_persist_header(&client->ctx, &client->current_header_field, val);
        client->current_header_field.data = NULL;
        client->current_header_field.len = 0;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
    }
    return 0;
}

/** @brief llhttp callback: all HTTP headers have been received.
 *
 * Flushes any remaining header field+value pair into the request context.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) to continue parsing. */
int
on_headers_complete(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    client->header_field_completed = 0;
    if (client->current_header_field.data) {
        static const csilk_str_view_t empty_val = {.data = "", .len = 0};
        const csilk_str_view_t*       val =
            client->current_header_value.data ? &client->current_header_value : &empty_val;
        _csilk_persist_header(&client->ctx, &client->current_header_field, val);
        client->current_header_field.data = NULL;
        client->current_header_field.len = 0;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
    }
    return 0;
}

/** @brief llhttp callback: body data received.
 *
 * Appends body data to the request body buffer (realloc as needed). Enforces
 * max_body_size limit (returns HPE_USER if exceeded). On realloc failure,
 * the existing body is freed and HPE_USER is returned.
 *
 * @param p      The llhttp parser instance.
 * @param at     Pointer to body data.
 * @param length Length of body data in bytes.
 * @return 0 (HPE_OK) on success, HPE_USER if the body exceeds max_body_size. */
int
on_body(llhttp_t* p, const char* at, size_t length)
{
    csilk_client_t* client = (csilk_client_t*)p->data;
    size_t          max_body = _csilk_server_get_max_body_size(client->server);
    if (client->ctx.request.body_len + length > max_body) {
        return HPE_USER;
    }

    /* Overflow check for size calculation: body_len + length + 1 */
    if (length > SIZE_MAX - client->ctx.request.body_len - 1) {
        return HPE_USER;
    }

    /* Zero-copy body: reference the data directly in the read buffer.
     * The body pointer is valid until the request completes (I/O
     * guarantees the buffer lifetime across the read callback). */
    if (client->ctx.request.body_len == 0) {
        /* First body chunk — store the direct reference. */
        client->ctx.request.body = (char*)at;
        client->ctx.request.body_len = length;
        client->ctx.request.body_capacity = 0;
        client->ctx.request.body_ownership = CSILK_OWN_BORROWED;
    } else if (client->ctx.request.body_ownership == CSILK_OWN_BORROWED &&
               client->ctx.request.body + client->ctx.request.body_len == at) {
        /* Contiguous body — extend the reference. */
        client->ctx.request.body_len += length;
    } else {
        /* Non-contiguous: must copy. Use worker/TLS body size-class pool. */
        size_t req_size = client->ctx.request.body_len + length + 1;
        if (client->ctx.request.body_ownership == CSILK_OWN_BORROWED) {
            size_t cap = 0;
            char*  new_body = (char*)csilk_body_alloc(req_size, &cap);
            if (!new_body) {
                client->ctx.request.body = NULL;
                client->ctx.request.body_len = 0;
                client->ctx.request.body_capacity = 0;
                return HPE_USER;
            }
            memcpy(new_body, client->ctx.request.body, client->ctx.request.body_len);
            memcpy(new_body + client->ctx.request.body_len, at, length);
            client->ctx.request.body_len += length;
            new_body[client->ctx.request.body_len] = '\0';
            client->ctx.request.body = new_body;
            client->ctx.request.body_capacity = cap;
            client->ctx.request.body_ownership = CSILK_OWN_POOL;
        } else {
            if (client->ctx.request.body_capacity >= req_size) {
                memcpy(client->ctx.request.body + client->ctx.request.body_len, at, length);
                client->ctx.request.body_len += length;
                client->ctx.request.body[client->ctx.request.body_len] = '\0';
            } else {
                size_t cap = 0;
                char*  new_body = NULL;
                new_body = (char*)csilk_body_realloc(client->ctx.request.body,
                                                     client->ctx.request.body_len,
                                                     client->ctx.request.body_capacity,
                                                     req_size,
                                                     &cap);
                if (!new_body) {
                    return HPE_USER;
                }
                memcpy(new_body + client->ctx.request.body_len, at, length);
                client->ctx.request.body_len += length;
                new_body[client->ctx.request.body_len] = '\0';
                client->ctx.request.body = new_body;
                client->ctx.request.body_capacity = cap;
            }
        }
    }
    return 0;
}

/* --- Request finalization --- */

/** @brief Finalize the parsed request data before routing.
 *
 * Stores any remaining header field+value pair, splits the URL into path
 * and query string, URL-decodes the path, parses query parameters into the
 * context's query_params map, and sets the HTTP method on the context.
 *
 * @param client The client connection.
 * @param p      The llhttp parser instance. */
static void
finalize_request(csilk_client_t* client, llhttp_t* p)
{
    /* Persist any remaining header field+value pair into the request context. */
    if (client->current_header_field.data) {
        static const csilk_str_view_t empty_val = {.data = "", .len = 0};
        const csilk_str_view_t*       val =
            client->current_header_value.data ? &client->current_header_value : &empty_val;
        _csilk_persist_header(&client->ctx, &client->current_header_field, val);
        client->current_header_field.data = NULL;
        client->current_header_field.len = 0;
        client->current_header_value.data = NULL;
        client->current_header_value.len = 0;
    }
    client->header_field_completed = 0;

    /* Process the URL: zero-copy fast arena allocation path */
    if (client->current_url.data && client->current_url.len > 0) {
        size_t      ulen = client->current_url.len;
        const char* udata = client->current_url.data;

        const char* qmark = memchr(udata, '?', ulen);
        if (__builtin_expect(!qmark, 1)) {
            char* path = csilk_arena_alloc(client->ctx.arena, ulen + 1);
            if (path) {
                memcpy(path, udata, ulen);
                path[ulen] = '\0';
                csilk_url_decode(path);
                client->ctx.request.path = path;
            }
        } else {
            size_t path_len = (size_t)(qmark - udata);
            char*  path = csilk_arena_alloc(client->ctx.arena, path_len + 1);
            if (path) {
                memcpy(path, udata, path_len);
                path[path_len] = '\0';
                csilk_url_decode(path);
                client->ctx.request.path = path;
            }
            size_t query_len = ulen - path_len - 1;
            char*  query = csilk_arena_alloc(client->ctx.arena, query_len + 1);
            if (query) {
                memcpy(query, qmark + 1, query_len);
                query[query_len] = '\0';
                csilk_parse_query(&client->ctx, query);
            }
        }
        client->current_url.data = NULL;
        client->current_url.len = 0;
    }

    enum llhttp_method m = (enum llhttp_method)llhttp_get_method(p);
    switch (m) {
    case HTTP_GET:
        client->ctx.request.method = "GET";
        break;
    case HTTP_POST:
        client->ctx.request.method = "POST";
        break;
    case HTTP_PUT:
        client->ctx.request.method = "PUT";
        break;
    case HTTP_DELETE:
        client->ctx.request.method = "DELETE";
        break;
    case HTTP_HEAD:
        client->ctx.request.method = "HEAD";
        break;
    case HTTP_OPTIONS:
        client->ctx.request.method = "OPTIONS";
        break;
    case HTTP_PATCH:
        client->ctx.request.method = "PATCH";
        break;
    default:
        client->ctx.request.method = (char*)llhttp_method_name(m);
        break;
    }
}

/* --- Message complete --- */

/** @brief llhttp callback: the full HTTP request message has been parsed.
 *
 * This is the main request dispatch point. It executes the following
 * pipeline for every incoming HTTP request:
 *
 *   1. finalize_request(): store remaining headers, split URL into path
 *      and query, URL-decode the path, parse query parameters.
 *
 *   2. _csilk_dispatch_request(): trigger hooks, match route, prepend
 *      global middlewares, execute handler chain, send response.
 *
 * @param p The llhttp parser instance.
 * @return 0 (HPE_OK) on success, non-zero to abort parsing. */
int
on_message_complete(llhttp_t* p)
{
    csilk_client_t* client = (csilk_client_t*)p->data;

    finalize_request(client, p);
    llhttp_pause(p);
    _csilk_dispatch_request(&client->ctx);

    return 0;
}
