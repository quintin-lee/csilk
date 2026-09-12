/**
 * @file server_docs.c
 * @brief Built-in docs & SPA handlers — OpenAPI/Swagger routes and static fallback.
 *
 * Hosts the request handlers that server_lifecycle.c used to own despite not
 * being lifecycle concerns: the auto-registered OpenAPI JSON / Swagger UI
 * endpoints and the SPA (Single Page Application) index.html fallback.
 * csilk_server_run() pulls the route registration in via
 * _csilk_server_register_docs_routes(), declared via srv_impl.h.
 * @copyright MIT License
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- SPA fallback --- */

/** @brief Built-in SPA (Single Page Application) fallback handler. */
static void
spa_fallback_handler(csilk_ctx_t* c)
{
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
    char* body = csilk_arena_calloc(c->arena, 1, (size_t)fsize + 1);
    if (!body) {
        fclose(f);
        csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
        return;
    }
    size_t nread = fread(body, 1, (size_t)fsize, f);
    fclose(f);
    csilk_set_header(c, "Content-Type", "text/html");
    csilk_set_response_body_ex(c, body, nread, CSILK_OWN_ARENA);
    csilk_status(c, CSILK_STATUS_OK);
}

/** @brief Set a custom handler for unmatched routes (404 Not Found). */
void
csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler)
{
    if (!server) {
        return;
    }
    server->not_found_handler = handler;
}

/** @brief Enable SPA fallback: all unmatched GET requests serve index.html. */
void
csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root)
{
    if (!server || !doc_root) {
        return;
    }
    free(server->spa_doc_root);
    server->spa_doc_root = strdup(doc_root);
    if (server->spa_doc_root) {
        server->not_found_handler = spa_fallback_handler;
    }
}

/* --- OpenAPI / Swagger docs --- */

/**
 * @brief Handler that serves the auto-generated OpenAPI JSON document.
 * @param[in] c Request context used to resolve the server/router and respond.
 * @note Looks up the owning server and router; on success calls
 *       csilk_serve_openapi, otherwise sets HTTP 500.
 */
static void
openapi_json_handler(csilk_ctx_t* c)
{
    csilk_server_t* server = csilk_ctx_get_server(c);
    if (server && server->router) {
        csilk_serve_openapi(
            c, server->router, "csilk API", CSILK_VERSION, "Auto-generated OpenAPI documentation.");
    } else {
        csilk_set_status(c, 500);
    }
}

/**
 * @brief Handler that serves the Swagger UI documentation page.
 * @param[in] c Request context.
 */
static void
swagger_docs_handler(csilk_ctx_t* c)
{
    csilk_serve_swagger_ui(c);
}

/**
 * @brief Register the auto-generated OpenAPI/Swagger routes when enabled.
 * @param[in] server Server whose router receives the docs endpoints.
 * @note Extracted verbatim from csilk_server_run(); registers GET
 *       /openapi.json, /docs, /swagger and /swagger-ui.
 */
void
_csilk_server_register_docs_routes(csilk_server_t* server)
{
    if (server->config.enable_openapi && server->router) {
        static csilk_handler_t openapi_handlers[] = {openapi_json_handler, NULL};
        static csilk_handler_t docs_handlers[] = {swagger_docs_handler, NULL};
        csilk_router_add(server->router, "GET", "/openapi.json", openapi_handlers, 1);
        csilk_router_add(server->router, "GET", "/docs", docs_handlers, 1);
        csilk_router_add(server->router, "GET", "/swagger", docs_handlers, 1);
        csilk_router_add(server->router, "GET", "/swagger-ui", docs_handlers, 1);
        CSILK_LOG_I("Server: OpenAPI and Swagger UI endpoints automatically registered at "
                    "/openapi.json, /docs, /swagger");
    }
}
