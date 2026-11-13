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
#include "../core/primitives/router_internal.h"

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
void
csilk_openapi_cache_invalidate(void)
{
    /* No-op: OpenAPI caching is maintained per-router on csilk_router_t */
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

    csilk_mutex_lock(&r->openapi_mutex);
    if (!r->openapi_cache || r->openapi_cache_routes != r->route_count) {
        if (r->openapi_cache) {
            free(r->openapi_cache);
            r->openapi_cache = NULL;
        }
        csilk_json_t* doc = csilk_generate_openapi_json(r, title, version, description);
        if (doc) {
            r->openapi_cache = csilk_json_serialize(doc, NULL);
            csilk_json_free(doc);
            r->openapi_cache_routes = r->route_count;
        }
    }
    const char* cached_json = r->openapi_cache;
    if (cached_json) {
        csilk_json_string(c, CSILK_STATUS_OK, cached_json);
    } else {
        csilk_json_error(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Failed to generate OpenAPI spec");
    }
    csilk_mutex_unlock(&r->openapi_mutex);
}

/* =========================================================================
 *  Embedded Swagger UI page
 * ========================================================================= */

/** @brief Compiled-in Swagger UI HTML page template.
 *
 * Provides self-contained API documentation rendering with local asset loading
 * and automatic CDN fallback for seamless operation when embedded in external
 * applications. */
static const char swagger_ui_html_template[] =
    "<!-- HTML for csilk Swagger UI with local and CDN fallback -->\n"
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>csilk API Documentation</title>\n"
    "<link rel=\"stylesheet\" href=\"/csilk-docs/swagger-ui.css\" "
    "onerror=\"this.onerror=null;this.href='https://unpkg.com/swagger-ui-dist@5/"
    "swagger-ui.css';\">\n"
    "<link rel=\"icon\" type=\"image/png\" href=\"/csilk-docs/favicon-32x32.png\" "
    "sizes=\"32x32\">\n"
    "<link rel=\"icon\" type=\"image/png\" href=\"/csilk-docs/favicon-16x16.png\" "
    "sizes=\"16x16\">\n"
    "<style>\n"
    "  html { box-sizing: border-box; overflow: -moz-scrollbars-vertical; overflow-y: scroll; }\n"
    "  *, *:before, *:after { box-sizing: inherit; }\n"
    "  body { margin: 0; background: #fafafa; }\n"
    "  .swagger-ui .topbar { display: none; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div id=\"swagger-ui\"></div>\n"
    "<script>\n"
    "function initSwaggerUI() {\n"
    "  if (typeof SwaggerUIBundle === 'undefined' || typeof SwaggerUIStandalonePreset === "
    "'undefined') {\n"
    "    return;\n"
    "  }\n"
    "  window.ui = SwaggerUIBundle({\n"
    "    url: \"%s\",\n"
    "    dom_id: \"#swagger-ui\",\n"
    "    deepLinking: true,\n"
    "    presets: [\n"
    "      SwaggerUIBundle.presets.apis,\n"
    "      SwaggerUIStandalonePreset\n"
    "    ],\n"
    "    plugins: [\n"
    "      SwaggerUIBundle.plugins.DownloadUrl\n"
    "    ],\n"
    "    layout: \"StandaloneLayout\",\n"
    "    showExtensions: true,\n"
    "    showCommonExtensions: true\n"
    "  });\n"
    "}\n"
    "function loadScript(src, fallbackSrc, cb) {\n"
    "  var s = document.createElement('script');\n"
    "  s.src = src;\n"
    "  s.onload = cb;\n"
    "  s.onerror = function() {\n"
    "    if (fallbackSrc) {\n"
    "      var fb = document.createElement('script');\n"
    "      fb.src = fallbackSrc;\n"
    "      fb.onload = cb;\n"
    "      fb.onerror = function() { console.error('Failed to load Swagger UI script:', "
    "fallbackSrc); };\n"
    "      document.head.appendChild(fb);\n"
    "    }\n"
    "  };\n"
    "  document.head.appendChild(s);\n"
    "}\n"
    "window.onload = function() {\n"
    "  loadScript('/csilk-docs/swagger-ui-bundle.js', "
    "'https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js', function() {\n"
    "    loadScript('/csilk-docs/swagger-ui-standalone-preset.js', "
    "'https://unpkg.com/swagger-ui-dist@5/swagger-ui-standalone-preset.js', function() {\n"
    "      initSwaggerUI();\n"
    "    });\n"
    "  });\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/** @brief Serve the embedded Swagger UI page with a custom OpenAPI spec URL.
 *  @param c        The request context.
 *  @param spec_url URL of the OpenAPI JSON specification (e.g. "/openapi.json").
 */
void
csilk_serve_swagger_ui_ext(csilk_ctx_t* c, const char* spec_url)
{
    if (!c) {
        return;
    }
    const char*    url = (spec_url && *spec_url) ? spec_url : "/openapi.json";
    csilk_arena_t* arena = csilk_get_arena(c);
    char* html = arena ? csilk_arena_alloc(arena, sizeof(swagger_ui_html_template) + 256) : NULL;
    if (html) {
        snprintf(html, sizeof(swagger_ui_html_template) + 256, swagger_ui_html_template, url);
        csilk_set_header(c, "Content-Type", "text/html; charset=utf-8");
        csilk_string(c, CSILK_STATUS_OK, html);
    } else {
        char buf[sizeof(swagger_ui_html_template) + 256];
        snprintf(buf, sizeof(buf), swagger_ui_html_template, url);
        csilk_set_header(c, "Content-Type", "text/html; charset=utf-8");
        csilk_string(c, CSILK_STATUS_OK, buf);
    }
}

/** @brief Serve the embedded Swagger UI page.
 *  The page loads /openapi.json at runtime to render interactive documentation.
 */
void
csilk_serve_swagger_ui(csilk_ctx_t* c)
{
    csilk_serve_swagger_ui_ext(c, "/openapi.json");
}
