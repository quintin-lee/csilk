/**
 * @file admin.h
 * @brief Unified administration and monitoring controller for the csilk
 * framework.
 *
 * Provides a web-based dashboard for real-time observability of HTTP metrics,
 * AI Workflows, and the Internal Message Queue.
 */

#ifndef CSILK_APP_ADMIN_H
#define CSILK_APP_ADMIN_H

#include "csilk/core/types.h"

struct csilk_app_s;
typedef struct csilk_app_s csilk_app_t;

/**
 * @brief Register administration routes to an application.
 *
 * This function registers several routes under the given @p path:
 * - GET <path>       : Serves the HTML dashboard UI.
 * - GET <path>/stats : Returns a JSON snapshot of all system metrics.
 * - GET <path>/ws    : WebSocket endpoint for real-time event streaming.
 *
 * @param app  The application handle.
 * @param path The base URL path for the admin panel (e.g., "/admin").
 *
 * @note Requires the server to be running and WebSocket support enabled.
 */
void csilk_admin_serve(csilk_app_t* app, const char* path);

/**
 * @brief Register protected administration routes to an application.
 *
 * Similar to csilk_admin_serve, but wraps all admin routes in the provided
 * @p auth_middleware. This ensures that the dashboard, stats, and WebSocket
 * endpoints are only accessible to authorized users.
 *
 * @param app             The application handle.
 * @param path            The base URL path for the admin panel.
 * @param auth_middleware The middleware to use for authentication.
 */
void csilk_admin_serve_secure(csilk_app_t* app, const char* path, csilk_handler_t auth_middleware);

#endif /* CSILK_APP_ADMIN_H */
