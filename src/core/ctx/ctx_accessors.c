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

/**
 * @brief Look up a path/route parameter value by key.
 * @param[in] c   Request context to search (validated non-NULL).
 * @param[in] key Parameter name (validated non-NULL).
 * @return The parameter value, or NULL if not found or args are invalid.
 */
const char*
csilk_get_param(csilk_ctx_t* c, const char* key)
{
    if (!c || !key) {
        return NULL;
    }
    for (int i = 0; i < c->params_count; i++) {
        if (strcmp(c->params[i].key, key) == 0) {
            return c->params[i].value;
        }
    }
    return NULL;
}

/**
 * @brief Get the number of path/route parameters.
 * @param[in] c Request context (may be NULL).
 * @return The parameter count, or 0 if c is NULL.
 */
int
csilk_get_params_count(csilk_ctx_t* c)
{
    return c ? c->params_count : 0;
}

/**
 * @brief Get the key of the path parameter at a given index.
 * @param[in] c     Request context (validated non-NULL).
 * @param[in] index Parameter index (must be within range).
 * @return The parameter key, or NULL if out of range or args invalid.
 */
const char*
csilk_get_param_key(csilk_ctx_t* c, int index)
{
    if (c && index >= 0 && index < c->params_count) {
        return c->params[index].key;
    }
    return NULL;
}

/**
 * @brief Get the value of the path parameter at a given index.
 * @param[in] c     Request context (validated non-NULL).
 * @param[in] index Parameter index (must be within range).
 * @return The parameter value, or NULL if out of range or args invalid.
 */
const char*
csilk_get_param_value(csilk_ctx_t* c, int index)
{
    if (c && index >= 0 && index < c->params_count) {
        return c->params[index].value;
    }
    return NULL;
}

/**
 * @brief Get a request header value by name.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] key Header name (validated non-NULL).
 * @return The header value, or NULL if absent.
 */
const char*
csilk_get_header(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->request.headers, key);
}

/**
 * @brief Get a response header value by name.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] key Header name (validated non-NULL).
 * @return The response header value, or NULL if absent.
 */
const char*
csilk_get_response_header(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->response.headers, key);
}

/**
 * @brief Get a query-string parameter value by name.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] key Parameter name (validated non-NULL).
 * @return The query parameter value, or NULL if absent.
 */
const char*
csilk_get_query(csilk_ctx_t* c, const char* key)
{
    return map_get(&c->request.query_params, key);
}

/**
 * @brief Set a request header key/value pair.
 * @param[in] c     Request context (validated non-NULL).
 * @param[in] key   Header name (validated non-NULL).
 * @param[in] value Header value (validated non-NULL).
 */
void
csilk_set_request_header(csilk_ctx_t* c, const char* key, const char* value)
{
    map_set(c, &c->request.headers, key, value);
}

/**
 * @brief Get the HTTP request method.
 * @param[in] c Request context (may be NULL).
 * @return The method string, or NULL if c is NULL.
 */
const char*
csilk_get_method(csilk_ctx_t* c)
{
    return c ? c->request.method : NULL;
}

/**
 * @brief Get the request path.
 * @param[in] c Request context (may be NULL).
 * @return The path string, or NULL if c is NULL.
 */
const char*
csilk_get_path(csilk_ctx_t* c)
{
    return c ? c->request.path : NULL;
}

/**
 * @brief Report whether the connection is a WebSocket.
 * @param[in] c Request context (may be NULL).
 * @return 1 if WebSocket, 0 otherwise or if c is NULL.
 */
int
csilk_is_websocket(csilk_ctx_t* c)
{
    return c ? c->is_websocket : 0;
}

/**
 * @brief Mark/unmark the connection as a WebSocket.
 * @param[in] c            Request context (validated non-NULL).
 * @param[in] is_websocket Non-zero to enable, zero to disable.
 */
void
csilk_ctx_set_websocket(csilk_ctx_t* c, int is_websocket)
{
    if (c) {
        c->is_websocket = is_websocket;
    }
}

/**
 * @brief Report whether the connection is Server-Sent Events (SSE).
 * @param[in] c Request context (may be NULL).
 * @return 1 if SSE, 0 otherwise or if c is NULL.
 */
int
csilk_is_sse(csilk_ctx_t* c)
{
    return c ? c->is_sse : 0;
}

/**
 * @brief Mark/unmark the connection as Server-Sent Events (SSE).
 * @param[in] c      Request context (validated non-NULL).
 * @param[in] is_sse Non-zero to enable, zero to disable.
 */
void
csilk_ctx_set_sse(csilk_ctx_t* c, int is_sse)
{
    if (c) {
        c->is_sse = is_sse;
    }
}

/**
 * @brief Get the opaque internal client pointer for a context.
 * @param[in] c Request context (may be NULL).
 * @return The internal client pointer (e.g. csilk_client_t*), or NULL.
 */
void*
_csilk_get_internal_client(csilk_ctx_t* c)
{
    return c ? c->_internal_client : NULL;
}

/**
 * @brief Set the opaque internal client pointer for a context.
 * @param[in] c      Request context (validated non-NULL).
 * @param[in] client Opaque client pointer (e.g. csilk_client_t*).
 */
void
_csilk_set_internal_client(csilk_ctx_t* c, void* client)
{
    if (c) {
        c->_internal_client = client;
    }
}

/**
 * @brief Get the request's unique ID string.
 * @param[in] c Request context (may be NULL).
 * @return The request ID, or NULL if c is NULL.
 */
const char*
csilk_get_request_id(csilk_ctx_t* c)
{
    return c ? c->request_id : NULL;
}

/**
 * @brief Get the memory arena backing a context.
 * @param[in] c Request context (may be NULL).
 * @return The arena, or NULL if c is NULL.
 */
csilk_arena_t*
csilk_get_arena(csilk_ctx_t* c)
{
    return c ? c->arena : NULL;
}

/**
 * @brief Get the response HTTP status code.
 * @param[in] c Request context (may be NULL).
 * @return The status code, or 0 if c is NULL.
 */
int
csilk_get_status(csilk_ctx_t* c)
{
    return c ? c->response.status : 0;
}

/**
 * @brief Get the request header map.
 * @param[in] c Request context (may be NULL).
 * @return Pointer to the request header map, or NULL if c is NULL.
 */
csilk_header_map_t*
csilk_get_headers(csilk_ctx_t* c)
{
    return c ? &c->request.headers : NULL;
}

/**
 * @brief Mark/unmark the context as asynchronous.
 * @param[in] c        Request context (validated non-NULL).
 * @param[in] is_async Non-zero to enable async, zero to disable.
 */
void
csilk_ctx_set_async(csilk_ctx_t* c, int is_async)
{
    if (c) {
        c->is_async = is_async;
    }
}

/**
 * @brief Get the owning server for a context.
 * @param[in] c Request context (may be NULL).
 * @return The server, or NULL if c is NULL.
 */
csilk_server_t*
csilk_ctx_get_server(csilk_ctx_t* c)
{
    return c ? (csilk_server_t*)c->server : NULL;
}

/**
 * @brief Get the server's message queue for a context.
 * @param[in] c Request context (may be NULL).
 * @return The message queue, or NULL if c or its server is NULL.
 */
csilk_mq_t*
csilk_ctx_get_mq(csilk_ctx_t* c)
{
    return (c && c->server) ? c->server->mq : NULL;
}

/**
 * @brief Report whether the context is asynchronous.
 * @param[in] c Request context (may be NULL).
 * @return 1 if async, 0 otherwise or if c is NULL.
 */
int
csilk_is_async(csilk_ctx_t* c)
{
    return c ? c->is_async : 0;
}

/**
 * @brief Get the index of the matched handler.
 * @param[in] c Request context (may be NULL).
 * @return The handler index, or -1 if c is NULL.
 */
int
csilk_get_handler_index(csilk_ctx_t* c)
{
    return c ? c->handler_index : -1;
}

/**
 * @brief Set the request's unique ID string.
 * @param[in] c  Request context (validated non-NULL).
 * @param[in] id Request ID (validated non-NULL); copied into c->request_id.
 */
void
csilk_set_request_id(csilk_ctx_t* c, const char* id)
{
    if (c && id) {
        snprintf(c->request_id, sizeof(c->request_id), "%s", id);
    }
}

/**
 * @brief Get the per-request work request handle.
 * @param[in] c Request context (may be NULL).
 * @return Pointer to the embedded work request, or NULL if c is NULL.
 */
csilk_io_work_t*
csilk_get_work_req(csilk_ctx_t* c)
{
    return c ? &c->work_req : NULL;
}

/**
 * @brief Get the matched handler's registered path.
 * @param[in] c Request context (may be NULL).
 * @return The handler path, or NULL if no handler is matched.
 */
const char*
csilk_ctx_get_handler_path(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->path : NULL;
}

/**
 * @brief Get the matched handler's required permission string.
 * @param[in] c Request context (may be NULL).
 * @return The permission requirement, or NULL if no handler is matched.
 */
const char*
csilk_ctx_get_handler_perm_required(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->perm_required : NULL;
}

/**
 * @brief Get the matched handler's permission resource string.
 * @param[in] c Request context (may be NULL).
 * @return The permission resource, or NULL if no handler is matched.
 */
const char*
csilk_ctx_get_handler_perm_resource(csilk_ctx_t* c)
{
    return (c && c->current_handler) ? c->current_handler->perm_resource : NULL;
}

/**
 * @brief Report whether the request has been aborted.
 * @param[in] c Request context (may be NULL).
 * @return 1 if aborted, 0 otherwise or if c is NULL.
 */
int
csilk_is_aborted(csilk_ctx_t* c)
{
    return c ? c->aborted : 0;
}

/**
 * @brief Iterate every entry of a header/param map, invoking a callback.
 * @param[in] map Header/param map to iterate (validated non-NULL).
 * @param[in] cb  Per-entry callback; iteration stops early if cb returns 0.
 * @param[in] arg Opaque argument passed to cb.
 * @note Walks all hash buckets; cb receives (key, value, arg).
 */
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

/**
 * @brief Iterate the request headers, invoking a callback per entry.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] cb  Per-header callback (receives key, value, arg).
 * @param[in] arg Opaque argument passed to cb.
 */
void
csilk_for_each_header(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.headers, cb, arg);
    }
}

/**
 * @brief Iterate the query parameters, invoking a callback per entry.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] cb  Per-param callback (receives key, value, arg).
 * @param[in] arg Opaque argument passed to cb.
 */
void
csilk_for_each_query(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.query_params, cb, arg);
    }
}

/**
 * @brief Iterate the form parameters, invoking a callback per entry.
 * @param[in] c   Request context (validated non-NULL).
 * @param[in] cb  Per-field callback (receives key, value, arg).
 * @param[in] arg Opaque argument passed to cb.
 */
void
csilk_for_each_form_field(csilk_ctx_t* c, csilk_header_cb cb, void* arg)
{
    if (c) {
        for_each_in_map(&c->request.form_params, cb, arg);
    }
}

/**
 * @brief Set the WebSocket message callback for a context.
 * @param[in] c        Request context (validated non-NULL).
 * @param[in] callback Callback invoked on incoming WebSocket frames.
 * @note Stores the callback in c->on_ws_message.
 */
void
csilk_set_on_ws_message(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
    if (c) {
        c->on_ws_message = callback;
    }
}

/**
 * @brief Set the WebSocket send callback for a context.
 * @param[in] c        Request context (validated non-NULL).
 * @param[in] callback Callback invoked when sending WebSocket frames.
 * @note Stores the callback in c->on_ws_send.
 */
void
csilk_set_on_ws_send(
    csilk_ctx_t* c,
    void (*callback)(csilk_ctx_t* c, const uint8_t* payload, size_t len, int opcode))
{
    if (c) {
        c->on_ws_send = callback;
    }
}

/**
 * @brief Get the WebSocket message callback registered for a context.
 * @param[in] c Request context (may be NULL).
 * @return The on_ws_message callback, or NULL if c is NULL.
 */
void (*csilk_get_on_ws_message(csilk_ctx_t* c))(csilk_ctx_t*   c,
                                                const uint8_t* payload,
                                                size_t         len,
                                                int            opcode)
{
    return c ? c->on_ws_message : NULL;
}
