/**
 * @file admin.h
 * @brief Unified administration and monitoring controller for the csilk framework.
 *
 * Provides a web-based dashboard for real-time observability of HTTP metrics,
 * AI Workflows, and the Internal Message Queue.
 */

#ifndef CSILK_ADMIN_H
#define CSILK_ADMIN_H

#include "csilk/csilk.h"
#include "csilk/app/app.h"

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

#endif /* CSILK_ADMIN_H */
