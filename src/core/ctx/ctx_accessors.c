/**
 * @file ctx_accessors.c
 * @brief Context getter/setter accessors, iteration, and WebSocket callbacks.
 *
 * Extracted from context.c to keep that file focused on lifecycle and body I/O.
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ctx_internal.h"
#include "../primitives/header_map.h"
#include "../primitives/query.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"

const char*
csilk_get_param(csilk_ctx_t* c, const char* key)
{
    if (!c || !key) {
        return nullptr;
    }
    for (int i = 0; i < c->params_count; i++) {
        if (strcmp(c->params[i].key, key) == 0) {
            return c->params[i].value;
        }
    }
    return nullptr;
}

int
csilk_get_params_count(csilk_ctx_t* c)
{
    return c ? c->params_count : 0;
}

const char*
csilk_get_param_key(csilk_ctx_t* c, int index)
{
    if (c && index >= 0 && index < c->params_count) {
        return c->params[index].key;
    }
    return nullptr;
}

const char*
csilk_get_param_value(csilk_ctx_t* c, int index)
{
    if (c && index >= 0 && index < c->params_count) {
        return c->params[index].value;
    }
    return nullptr;
}

const char*
csilk_get_header(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->request.headers, key);
}

const char*
csilk_get_response_header(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->response.headers, key);
}

const char*
csilk_get_query(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->request.query_params, key);
}

void
csilk_set_request_header(csilk_ctx_t* c, const char* key, const char* value)
{
    map_set(c, &c->request.headers, key, value);
}

const char*
csilk_get_method(csilk_ctx_t* c)
{
    return c ? c->request.method : nullptr;
}

const char*
csilk_get_path(csilk_ctx_t* c)
{
    return c ? c->request.path : nullptr;
}

int
csilk_is_websocket(csilk_ctx_t* c)
{
    return c ? c->is_websocket : 0;
}

void
csilk_ctx_set_websocket(csilk_ctx_t* c, int is_websocket)
{
    if (c) {
        c->is_websocket = is_websocket;
    }
}

int
csilk_is_sse(csilk_ctx_t* c)
{
    return c ? c->is_sse : 0;
}

void
csilk_ctx_set_sse(csilk_ctx_t* c, int is_sse)
{
    if (c) {
        c->is_sse = is_sse;
    }
}

void*
_csilk_get_internal_client(csilk_ctx_t* c)
{
    return c ? c->_internal_client : nullptr;
}

void
_csilk_set_internal_client(csilk_ctx_t* c, void* client)
{
    if (c) {
        c->_internal_client = client;
    }
}

const char*
csilk_get_request_id(csilk_ctx_t* c)
{
    return c ? c->request_id : nullptr;
}

csilk_arena_t*
csilk_get_arena(csilk_ctx_t* c)
{
    return c ? c->arena : nullptr;
}

int
csilk_get_status(csilk_ctx_t* c)
{
    return c ? c->response.status : 0;
}

csilk_header_map_t*
csilk_get_headers(csilk_ctx_t* c)
{
    return c ? &c->request.headers : nullptr;
}

void
csilk_ctx_set_async(csilk_ctx_t* c, int is_async)
{
    if (c) {
        c->is_async = is_async;
    }
}

csilk_server_t*
csilk_ctx_get_server(csilk_ctx_t* c)
{
    return c ? (csilk_server_t*)c->server : nullptr;
}

csilk_mq_t*
csilk_ctx_get_mq(csilk_ctx_t* c)
{
    return (c && c->server) ? c->server->mq : nullptr;
}

int
csilk_is_async(csilk_ctx_t* c)
{
    return c ? c->is_async : 0;
}

int
csilk_get_handler_index(csilk_ctx_t* c)
{
    return c ? c->handler_index : -1;
}

void
csilk_set_request_id(csilk_ctx_t* c, const char* id)
{
    if (c && id) {
        snprintf(c->request_id, sizeof(c->request_id), "%s", id);
    }
}

csilk_io_work_t*
csilk_get_work_req(csilk_ctx_t* c)
{
    return c ? &c->work_req : nullptr;
}

const char*
csilk_ctx_get_handler_path(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->path : nullptr;
}

const char*
csilk_ctx_get_handler_perm_required(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->perm_required : nullptr;
}

const char*
csilk_ctx_get_handler_perm_resource(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->perm_resource : nullptr;
}

int
csilk_is_aborted(csilk_ctx_t* c)
{
    return c ? c->aborted : 0;
}

static void
for_each_in_map(csilk_header_map_t* map, csilk_header_cb cb, void* arg)
{
    if (!map || !cb) {
        return;
    }
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        csilk_header_t* h = map->buckets[i];
        while (h) {
            if (!cb(h->key, h->value, arg)) {
                return;
            }
            h = h->next;
        }
    }
}

void
csilk_for_each_header(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.headers, cb, arg);
    }
}

void
csilk_for_each_query(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.query_params, cb, arg);
    }
}

void
csilk_for_each_form_field(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.form_params, cb, arg);
    }
}

void
csilk_set_on_ws_message(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
    if (c) {
        c->on_ws_message = callback;
    }
}

void
csilk_set_on_ws_send(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
    if (c) {
        c->on_ws_send = callback;
    }
}

void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t*   c,
                                                const uint8_t* payload,
                                                size_t         len,
                                                int            opcode)
{
    return c ? c->on_ws_message : nullptr;
}
