/**
 * @file hooks.h
 * @brief Lifecycle hook system for the csilk server framework.
 *
 * Hooks allow users to inject custom logic at well-defined points in the
 * server and request lifecycle without modifying the framework code.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#ifndef CSILK_HOOKS_H
#define CSILK_HOOKS_H

#include "csilk/core/types.h"

/**
 * @brief Lifecycle hook types for the server and individual requests.
 *
 * Hooks allow users to inject custom logic at well-defined points in the
 * server and request lifecycle without modifying the framework code.
 */
typedef enum {
    CSILK_HOOK_SERVER_START,  /**< Invoked just before the event loop starts
                               (server-level). */
    CSILK_HOOK_SERVER_STOP,   /**< Invoked when the server is shutting down
                               (server-level). */
    CSILK_HOOK_CONN_OPEN,     /**< Invoked when a new TCP connection is accepted
                               (context-level). */
    CSILK_HOOK_CONN_CLOSE,    /**< Invoked when a TCP connection is closed
                               (context-level). */
    CSILK_HOOK_REQUEST_BEGIN, /**< Invoked when the full HTTP request has been
                               parsed (context-level). */
    CSILK_HOOK_REQUEST_END,   /**< Invoked after the response has been sent
                               (context-level). */
    CSILK_HOOK_COUNT          /**< Sentinel — total number of hook types. Not a valid hook
                      type. */
} csilk_hook_type_t;

/**
 * @brief Callback signature for server-level hooks.
 *
 * @param s The server instance.
 */
typedef void (*csilk_server_hook_handler_t)(csilk_server_t* s);

/**
 * @brief Callback signature for request/connection-level hooks.
 *
 * @param c The request context.
 */
typedef void (*csilk_ctx_hook_handler_t)(csilk_ctx_t* c);

/**
 * @brief Register a lifecycle hook callback.
 *
 * The @p handler is cast to the appropriate type internally based on
 * @p type.  Multiple handlers may be registered for the same hook type.
 *
 * @param s       The server instance.
 * @param type    The hook type (see csilk_hook_type_t).
 * @param handler Pointer to the callback function.  Must match the expected
 *                signature for @p type (csilk_server_hook_handler_t for
 *                SERVER_*, csilk_ctx_hook_handler_t for others).
 */
void csilk_server_add_hook(csilk_server_t* s, csilk_hook_type_t type, void* handler);

#endif /* CSILK_HOOKS_H */
