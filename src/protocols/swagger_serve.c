/**
 * @file swagger_serve.c
 * @brief HTTP serving of Swagger UI assets and OpenAPI spec.
 *
 * The Swagger UI page is compiled into the binary as a static string and
 * served at a designated route by csilk_serve_swagger_ui(). It loads the
 * /openapi.json endpoint at runtime. The OpenAPI spec itself is generated
 * on demand (with caching) by csilk_serve_openapi().
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/reflection/reflect.h"

/* Forward declaration — defined in openapi_gen.c */
extern csilk_json_t* csilk_generate_openapi_json(csilk_router_t* router,
                                                 const char*     title,
                                                 const char*     version,
                                                 const char*     description);

/** @brief Serve the generated OpenAPI 3.0 spec as a JSON response.
 *
 * Intended to be called from within a route handler. Generates the OpenAPI
 * document via csilk_generate_openapi_json() and sends it as a JSON response.
 *
 * @param c           The request context.
 * @param r           The router instance.
 * @param title       API title.
 * @param version     API version.
 * @param description API description.
 * @note The response is sent synchronously via csilk_json(). On failure, a
 *       500 error response is sent. */
static char*         g_openapi_cache_json = nullptr;
static csilk_once_t  g_openapi_cache_once = CSILK_ONCE_INIT;
static csilk_mutex_t g_openapi_cache_mutex;

static void
init_openapi_cache_mutex(void)
{
    csilk_mutex_init(&g_openapi_cache_mutex);
}

void
csilk_serve_openapi(csilk_ctx_t*    c,
                    csilk_router_t* r,
                    const char*     title,
                    const char*     version,
                    const char*     description)
{
    if (!c || !r) {
        return;
    }

    csilk_once(&g_openapi_cache_once, init_openapi_cache_mutex);

    csilk_mutex_lock(&g_openapi_cache_mutex);
    if (!g_openapi_cache_json) {
        csilk_json_t* doc = csilk_generate_openapi_json(r, title, version, description);
        if (!doc) {
            fprintf(stderr, "[DEBUG] csilk_generate_openapi_json returned NULL\n");
            fflush(stderr);
        }
        if (doc) {
            g_openapi_cache_json = csilk_json_serialize(doc, NULL);
            if (!g_openapi_cache_json) {
                fprintf(stderr, "[DEBUG] csilk_json_serialize returned NULL\n");
                fflush(stderr);
            }
            csilk_json_free(doc);
        }
    }
    const char* cached_json = g_openapi_cache_json;
    csilk_mutex_unlock(&g_openapi_cache_mutex);

    if (cached_json) {
        csilk_json_string(c, CSILK_STATUS_OK, cached_json);
    } else {
        csilk_json_error(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Failed to generate OpenAPI spec");
    }
}

/* =========================================================================
 *  Embedded Swagger UI page
 * ========================================================================= */

/** @brief Compiled-in Swagger UI HTML page.
 *
 * This HTML string is embedded directly into the binary and served at a
 * designated route by csilk_serve_swagger_ui().  It initializes the Swagger
 * UI JavaScript bundle to render an interactive API reference.
 *
 * Loading strategy:
 *   - CSS/JS assets are hosted at "/csilk-docs/" — these are static files
 *     from the official Swagger UI distribution bundled with the server
 *     (either as separate files or embedded via a build step).
 *   - The spec URL is "/openapi.json", served by csilk_serve_openapi().
 *     This decouples UI from spec generation: the UI HTML is loaded once,
 *     and the spec is fetched on page load, always reflecting the latest
 *     routes and types without a server restart.
 *   - SwaggerUIBundle + SwaggerUIStandalonePreset provide the full "Try it
 *     out" feature, response preview, and schema exploration. */
static const char swagger_ui_html[] =
    "<!-- HTML for static distribution bundle build -->\n"
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>csilk API Documentation</title>\n"
    "<link rel=\"stylesheet\" href=\"/csilk-docs/swagger-ui.css\">\n"
    "<link rel=\"stylesheet\" href=\"/csilk-docs/index.css\">\n"
    "<link rel=\"icon\" type=\"image/png\" "
    "href=\"/csilk-docs/favicon-32x32.png\" sizes=\"32x32\">\n"
    "<link rel=\"icon\" type=\"image/png\" "
    "href=\"/csilk-docs/favicon-16x16.png\" sizes=\"16x16\">\n"
    "</head>\n"
    "<body style=\"margin:0\">\n"
    "<div id=\"swagger-ui\"></div>\n"
    "<script src=\"/csilk-docs/swagger-ui-bundle.js\"></script>\n"
    "<script src=\"/csilk-docs/swagger-ui-standalone-preset.js\"></script>\n"
    "<script>\n"
    "window.onload=function(){\n"
    "  window.ui = SwaggerUIBundle({\n"
    "    url:\"/openapi.json\",\n"
    "    dom_id:\"#swagger-ui\",\n"
    "    deepLinking:true,\n"
    "    presets:[\n"
    "      SwaggerUIBundle.presets.apis,\n"
    "      SwaggerUIStandalonePreset\n"
    "    ],\n"
    "    plugins:[\n"
    "      SwaggerUIBundle.plugins.DownloadUrl\n"
    "    ],\n"
    "    layout:\"StandaloneLayout\",\n"
    "    showExtensions:true,\n"
    "    showCommonExtensions:true\n"
    "  });\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/** @brief Serve the embedded Swagger UI page.
 *  The page loads /openapi.json at runtime to render interactive documentation.
 */
void
csilk_serve_swagger_ui(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }
    csilk_set_header(c, "Content-Type", "text/html; charset=utf-8");
    csilk_string(c, CSILK_STATUS_OK, swagger_ui_html);
}
