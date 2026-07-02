/**
 * @file hooks.c
 * @brief Lifecycle hook registration and triggering for csilk.
 *
 * Hooks provide extension points at key moments in the server and request
 * lifecycle: server start/stop, connection open/close, request begin/end.
 * Multiple handlers can be registered per hook type; they fire in LIFO
 * order (most recently registered first).
 *
 * @copyright MIT License
 */

#include <stdlib.h>

#include "csilk/csilk.h"
#include "core/srv_internal.h"

/** @brief Register a lifecycle hook on the server.
 *
 * Hooks are invoked at specific points in the request lifecycle
 * (conn_open, conn_close, request_begin, request_end, server_start,
 * server_stop). Multiple handlers can be registered for the same hook type;
 * they are called in reverse order of registration (LIFO).
 *
 * @param s       The server instance.
 * @param type    Hook type (CSILK_HOOK_CONN_OPEN through CSILK_HOOK_SERVER_STOP).
 * @param handler Function pointer. For server hooks (start/stop), the
 *                signature is void(*)(csilk_server_t*). For context hooks,
 *                the signature is void(*)(csilk_ctx_t*). */
void
csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler)
{
    if (!s || type < 0 || type >= CSILK_HOOK_COUNT || !handler) {
        return;
    }

    csilk_hook_node_t* node = malloc(sizeof(csilk_hook_node_t));
    if (!node) {
        return;
    }

    node->handler = handler;
    node->next = s->hooks[type];
    s->hooks[type] = node;
}

/** @brief Internal: invoke all registered handlers for a given hook type.
 *
 * Walks the hook's linked list and calls each handler. Server-level hooks
 * (start/stop) receive the server pointer. Context-level hooks receive the
 * request context pointer.
 *
 * @param s    The server instance.
 * @param c    The request context (may be nullptr for server-level hooks).
 * @param type Hook type to trigger. */
CSILK_INTERNAL void
_csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type)
{
    if (!s || type < 0 || type >= CSILK_HOOK_COUNT) {
        return;
    }

    csilk_hook_node_t* curr = s->hooks[type];
    while (curr) {
        if (type <= CSILK_HOOK_SERVER_STOP) {
            ((csilk_server_hook_handler_t)curr->handler)(s);
        } else {
            if (c) {
                ((csilk_ctx_hook_handler_t)curr->handler)(c);
            }
        }
        curr = curr->next;
    }
}
