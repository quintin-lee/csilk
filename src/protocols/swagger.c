/**
 * @file swagger.c
 * @brief OpenAPI 3.0 specification generator and Swagger UI serving.
 *
 * Architecture:
 *   The OpenAPI document builder (csilk_generate_openapi_json) works in three
 *   phases: (1) traversing the route table to populate the "paths" section with
 *   endpoint descriptions, parameters, request bodies, and responses; (2)
 * calling add_schema() on each unique input/output type to populate
 * "components/schemas" with JSON Schema objects derived from the reflection
 * registry; (3) scanning all registered reflection types via
 * csilk_reflect_foreach() to ensure even orphan types appear in the spec.
 *
 *   Path patterns from the router (":param", "*glob") are converted to OpenAPI
 *   3.0 syntax ("{param}", "{glob+}") by path_to_openapi().  Schema generation
 *   uses generate_schema_for_type() to walk each reflection type's field
 * descriptors and emit "type" + "properties" maps, with nested structs emitting
 * "$ref" references.  Circular type references are handled by add_schema()'s
 * cycle detection (checks type presence before recursing).
 *
 *   The Swagger UI page is compiled into the binary as a static string and
 * served at a designated route by csilk_serve_swagger_ui().  It loads the
 * /openapi.json endpoint at runtime.
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

/** @brief Convert a csilk path pattern to OpenAPI 3.0 path format.
 *
 * Transforms ":param" segments to "{param}" and "*wildcard" segments to
 * "{wildcard+}" as required by the OpenAPI 3.0 specification.
 *
 * @param path     Input path pattern (e.g., "/users/:id/posts/star-path").
 * @param out      Output buffer for the OpenAPI-formatted path.
 * @param out_size Size of the output buffer (including null terminator).
 * @note The output is truncated to fit out_size if the converted path is
 *       too long. The caller should ensure out_size is large enough. */
static void
path_to_openapi(const char* path, char* out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        return;
    }
    const char* src = path;
    char*       dst = out;
    size_t      remaining = out_size - 1;

    /*
   * Walk the path character-by-character, converting router parameter syntax
   * to OpenAPI 3.0 path template syntax:
   *   Router ":param"    → OpenAPI "{param}"     (named path segment)
   * Router "*wildcard" → OpenAPI "{wildcard+}" (catch-all, greedy suffix)
   *
   * Example: "/users/:id/posts/...rest" → "/users/{id}/posts/{rest+}"
   */
    while (*src && remaining > 0) {
        if (*src == ':') {
            src++;
            if (remaining < 2) {
                break;
            }
            *dst++ = '{';
            remaining--;
            while (*src && *src != '/' && *src != '\0' && remaining > 1) {
                *dst++ = *src++;
                remaining--;
            }
            if (remaining < 2) {
                break;
            }
            *dst++ = '}';
            remaining--;
        } else if (*src == '*') {
            src++;
            if (remaining < 2) {
                break;
            }
            *dst++ = '{';
            remaining--;
            while (*src && *src != '\0' && remaining > 1) {
                *dst++ = *src++;
                remaining--;
            }
            if (remaining < 6) {
                break;
            }
            memcpy(dst, "+}", 2);
            dst += 2;
            remaining -= 2;
        } else {
            *dst++ = *src++;
            remaining--;
        }
    }
    *dst = '\0';
}

/** @brief Map a csilk reflection field type to an OpenAPI 3.0 schema type
 * string.
 *
 * Integer types map to "integer", float/double to "number", bool to "boolean",
 * string to "string", and struct to "object". Any unrecognized type defaults
 * to "string".
 *
 * @param type The csilk_field_type_t enum value.
 * @return A static string literal suitable for OpenAPI's "type" field. */
static const char*
field_type_to_openapi_type(csilk_field_type_t type)
{
    switch (type) {
    case CSILK_TYPE_INT8:
    case CSILK_TYPE_UINT8:
    case CSILK_TYPE_INT16:
    case CSILK_TYPE_UINT16:
    case CSILK_TYPE_INT32:
    case CSILK_TYPE_UINT32:
    case CSILK_TYPE_INT64:
    case CSILK_TYPE_UINT64:
        return "integer";
    case CSILK_TYPE_FLOAT:
    case CSILK_TYPE_DOUBLE:
        return "number";
    case CSILK_TYPE_BOOL:
        return "boolean";
    case CSILK_TYPE_STRING:
        return "string";
    case CSILK_TYPE_STRUCT:
        return "object";
    default:
        return "string";
    }
}

/** @brief Generate an OpenAPI 3.0 schema object for a registered reflection
 * type.
 *
 * Iterates the type's field descriptors and produces a schema with
 * "type": "object" and a "properties" map. Each field is mapped to its
 * OpenAPI type, with nested structs rendered as $ref pointers to
 * #/components/schemas/<type_name>. Array fields use an "array" type
 * wrapper with "items".
 *
 * @param type_name Registered reflection type name.
 * @return A cJSON object representing the OpenAPI schema, or nullptr if the
 *         type is not registered or allocation fails.
 * @note The caller must free the returned cJSON with cJSON_Delete(). */
static cJSON*
generate_schema_for_type(const char* type_name)
{
    const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
    if (!entry) {
        return nullptr;
    }

    cJSON* schema = cJSON_CreateObject();
    if (!schema) {
        return nullptr;
    }

    cJSON_AddStringToObject(schema, "type", "object");
    cJSON* properties = cJSON_AddObjectToObject(schema, "properties");
    if (!properties) {
        cJSON_Delete(schema);
        return nullptr;
    }

    /*
   * Walk each field descriptor registered for this reflection type and
   * produce a JSON Schema property entry:
   *
   *   1. Map csilk_field_type_t → OpenAPI type string via
   * field_type_to_openapi_type(). Integer types → "integer", float/double →
   * "number", bool → "boolean", string → "string", struct → "object".
   *
   *   2. Nested struct (CSILK_TYPE_STRUCT): emit a "$ref" pointer to the
   *      named component schema instead of inlining the object.  This keeps
   *      the spec DRY and enables recursive/circular type references.
   *      Example: {"$ref": "#/components/schemas/User"}.
   *
   *   3. Array fields (array_length > 0): wrap the property schema in an
   *      "array" type wrapper with "items".  Example:
   *      {"type": "array", "items": {"$ref": "#/components/schemas/Tag"}}.
   *
   *   4. Scalars: emit {"type": "<openapi-type>"} directly.
   */
    for (size_t i = 0; i < entry->count; i++) {
        const csilk_field_desc_t* field = &entry->fields[i];
        cJSON*                    prop = cJSON_CreateObject();

        const char* oa_type = field_type_to_openapi_type(field->type);
        if (field->type == CSILK_TYPE_STRUCT) {
            const char* type_name = field->nested_type_name ? field->nested_type_name : "unknown";
            char        ref[256];
            snprintf(ref, sizeof(ref), "#/components/schemas/%s", type_name);
            cJSON_AddStringToObject(prop, "$ref", ref);
        } else {
            cJSON_AddStringToObject(prop, "type", oa_type);
        }

        if (field->array_length > 0) {
            cJSON* arr_wrap = cJSON_CreateObject();
            cJSON_AddItemToObject(arr_wrap, "items", prop);
            cJSON_AddStringToObject(arr_wrap, "type", "array");
            cJSON_AddItemToObject(properties, field->json_key, arr_wrap);
        } else {
            cJSON_AddItemToObject(properties, field->json_key, prop);
        }
    }

    return schema;
}

/** @brief Internal: add a schema definition to the components/schemas map.
 *
 * If the schema for @p type_name already exists in @p schemas, this is a
 * no-op (prevents infinite recursion on circular type references). Otherwise
 * generates the schema, adds it, and recursively registers schemas for any
 * nested struct fields.
 *
 * @param schemas   The "schemas" cJSON object under components.
 * @param type_name Type name to generate and add. */
static void
add_schema(cJSON* schemas, const char* type_name)
{
    if (!schemas || !type_name || *type_name == '\0') {
        return;
    }

    /*
   * Cycle detection: if this type's schema is already in the schemas object,
   * return immediately.  This prevents infinite recursion on circular type
   * references (e.g., type A has a field of type B, and type B has a field
   * of type A).
   *
   * The schemas cJSON object doubles as a "visited" set: we insert the
   * schema upfront (cJSON_AddItemToObject) before recursing into nested
   * types, so a re-encounter is caught immediately.  The "$ref" pointer
   * in the parent schema correctly references the already-inserted entry.
   */
    if (cJSON_GetObjectItem(schemas, type_name)) {
        return;
    }

    cJSON* schema = generate_schema_for_type(type_name);
    if (schema) {
        cJSON_AddItemToObject(schemas, type_name, schema);

        // Recursively register schemas for nested struct types
        const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
        if (entry) {
            for (size_t i = 0; i < entry->count; i++) {
                if (entry->fields[i].type == CSILK_TYPE_STRUCT &&
                    entry->fields[i].nested_type_name) {
                    add_schema(schemas, entry->fields[i].nested_type_name);
                }
            }
        }
    }
}

/** @brief Internal: callback for csilk_reflect_foreach() to auto-register a
 * schema.
 *
 * Calls add_schema() for every type found during iteration. Used to ensure
 * all registered types appear in components/schemas even if not explicitly
 * linked to any route.
 *
 * @param name      Type name.
 * @param entry     Reflection entry (unused).
 * @param user_data Pointer to the schemas cJSON object. */
static void
auto_register_schema(const char* name, const csilk_reflect_entry_t* entry, void* user_data)
{
    (void)entry;
    add_schema((cJSON*)user_data, name);
}

/** @brief Generate a complete OpenAPI 3.0 specification document from the
 * router and reflection registry.
 *
 * Builds the full OpenAPI JSON structure including:
 * - openapi version field ("3.0.3")
 * - info section (title, version, description)
 * - paths section (one entry per route, with parameters, requestBody, and
 * responses)
 * - components/schemas section (auto-generated from all registered reflection
 * types)
 *
 * Path parameters are extracted from the route patterns and converted to
 * OpenAPI format. Request/response schemas are generated from input_type
 * and output_type metadata using the reflection engine.
 *
 * @param router      The router instance containing registered routes.
 * @param title       API title for the info section (pass nullptr for default).
 * @param version     API version string (pass nullptr for default "1.0.0").
 * @param description API description (may be nullptr).
 * @return A cJSON object representing the full OpenAPI document. Caller must
 *         free with cJSON_Delete(). Returns nullptr if router is nullptr or
 *         allocation fails. */
cJSON*
csilk_generate_openapi_json(csilk_router_t* router,
                            const char*     title,
                            const char*     version,
                            const char*     description)
{
    if (!router) {
        return nullptr;
    }

    cJSON* doc = cJSON_CreateObject();
    if (!doc) {
        return nullptr;
    }

    // OpenAPI version
    cJSON_AddStringToObject(doc, "openapi", "3.0.3");

    // Info section
    cJSON* info = cJSON_AddObjectToObject(doc, "info");
    if (info) {
        cJSON_AddStringToObject(info, "title", title ? title : "csilk API");
        cJSON_AddStringToObject(info, "version", version ? version : "1.0.0");
        if (description) {
            cJSON_AddStringToObject(info, "description", description);
        }
    }

    // Paths section
    cJSON* paths = cJSON_AddObjectToObject(doc, "paths");

    // Components section
    cJSON* components = cJSON_AddObjectToObject(doc, "components");
    cJSON* schemas = nullptr;
    if (components) {
        schemas = cJSON_AddObjectToObject(components, "schemas");
    }

    // Collect all routes
    cJSON* routes = csilk_router_collect_routes(router);
    if (!routes) {
        cJSON_Delete(doc);
        return nullptr;
    }

    /*
   * Phase 1 — Build the "paths" section by iterating every registered route.
   * Each route from csilk_router_collect_routes() carries method, path,
   * input_type, output_type, summary, and description as cJSON properties.
   *
   * Steps per route:
   *   a) Convert router-style path (":param") to OpenAPI syntax ("{param}").
   *   b) Get or create the path-level object in the "paths" map (multiple
   *      methods may share the same path, e.g. GET /users and POST /users).
   *   c) Add a method-level operation object ("get", "post", etc.) with
   *      summary, description, and operationId.
   *   d) Extract path parameters (":param" and "*wildcard") from the raw
   *      path pattern and emit them as OpenAPI parameter objects with
   *      "in": "path", "required": true.
   *   e) If the route has an input_type, add a "requestBody" with a "$ref"
   *      to the component schema and also register that schema.
   *   f) Add a "200" response with the output type's schema "$ref", and
   *      generic "400" and "500" error responses.
   */
    cJSON* route;
    cJSON_ArrayForEach(route, routes)
    {
        cJSON* method_item = cJSON_GetObjectItem(route, "method");
        cJSON* path_item = cJSON_GetObjectItem(route, "path");
        cJSON* input_item = cJSON_GetObjectItem(route, "input_type");
        cJSON* output_item = cJSON_GetObjectItem(route, "output_type");
        cJSON* summary_item = cJSON_GetObjectItem(route, "summary");
        cJSON* desc_item = cJSON_GetObjectItem(route, "description");

        if (!method_item || !path_item) {
            continue;
        }

        const char* method = cJSON_GetStringValue(method_item);
        const char* raw_path = cJSON_GetStringValue(path_item);
        if (!method || !raw_path) {
            continue;
        }

        // Convert path to OpenAPI format
        char oa_path[1024];
        path_to_openapi(raw_path, oa_path, sizeof(oa_path));

        // Get or create path item
        cJSON* path_obj = cJSON_GetObjectItem(paths, oa_path);
        if (!path_obj) {
            path_obj = cJSON_AddObjectToObject(paths, oa_path);
        }
        if (!path_obj) {
            continue;
        }

        // Method can be in lowercase for path item
        char   method_lower[16];
        size_t mlen = strlen(method);
        if (mlen >= sizeof(method_lower)) {
            mlen = sizeof(method_lower) - 1;
        }
        for (size_t i = 0; i < mlen; i++) {
            method_lower[i] = (char)tolower((unsigned char)method[i]);
        }
        method_lower[mlen] = '\0';

        // Check if method already exists (e.g., GET already added for this path)
        if (cJSON_GetObjectItem(path_obj, method_lower)) {
            continue;
        }

        cJSON* operation = cJSON_AddObjectToObject(path_obj, method_lower);
        if (!operation) {
            continue;
        }

        // Summary and description
        cJSON_AddStringToObject(operation,
                                "summary",
                                summary_item && cJSON_GetStringValue(summary_item)
                                    ? cJSON_GetStringValue(summary_item)
                                    : "");
        cJSON_AddStringToObject(
            operation,
            "description",
            desc_item && cJSON_GetStringValue(desc_item) ? cJSON_GetStringValue(desc_item) : "");
        cJSON_AddStringToObject(operation, "operationId", "");

        // Add Operation ID: method_path
        {
            char opid[1024];
            snprintf(opid, sizeof(opid), "%s%s", method, oa_path);
            cJSON_SetValuestring(cJSON_GetObjectItem(operation, "operationId"), opid);
        }

        // Parameters (path params extracted from path pattern)
        cJSON* params = cJSON_AddArrayToObject(operation, "parameters");

        // Extract path parameters from raw path
        const char* p = raw_path;
        while (*p) {
            if (*p == ':') {
                p++;
                const char* start = p;
                while (*p && *p != '/') {
                    p++;
                }
                size_t len = (size_t)(p - start);

                cJSON* param = cJSON_CreateObject();
                cJSON_AddStringToObject(param, "name", "");
                {
                    char   param_name[128];
                    size_t clen = len < sizeof(param_name) - 1 ? len : sizeof(param_name) - 1;
                    memcpy(param_name, start, clen);
                    param_name[clen] = '\0';
                    cJSON_SetValuestring(cJSON_GetObjectItem(param, "name"), param_name);
                }
                cJSON_AddStringToObject(param, "in", "path");
                cJSON_AddBoolToObject(param, "required", 1);
                cJSON* schema_obj = cJSON_AddObjectToObject(param, "schema");
                if (schema_obj) {
                    cJSON_AddStringToObject(schema_obj, "type", "string");
                }
                cJSON_AddItemToArray(params, param);
            } else if (*p == '*') {
                p++;
                const char* start = p;
                while (*p) {
                    p++;
                }
                size_t len = (size_t)(p - start);

                cJSON* param = cJSON_CreateObject();
                cJSON_AddStringToObject(param, "name", "");
                {
                    char   param_name[128];
                    size_t clen = len < sizeof(param_name) - 1 ? len : sizeof(param_name) - 1;
                    memcpy(param_name, start, clen);
                    param_name[clen] = '\0';
                    cJSON_SetValuestring(cJSON_GetObjectItem(param, "name"), param_name);
                }
                cJSON_AddStringToObject(param, "in", "path");
                cJSON_AddBoolToObject(param, "required", 1);
                cJSON* schema_obj = cJSON_AddObjectToObject(param, "schema");
                if (schema_obj) {
                    cJSON_AddStringToObject(schema_obj, "type", "string");
                }
                cJSON_AddItemToArray(params, param);
            } else {
                p++;
            }
        }

        // Request body (if input_type is set)
        const char* input_type = input_item ? cJSON_GetStringValue(input_item) : nullptr;
        if (input_type && *input_type != '\0') {
            // Add schema for this type
            if (schemas) {
                add_schema(schemas, input_type);
            }

            cJSON* req_body = cJSON_AddObjectToObject(operation, "requestBody");
            if (req_body) {
                cJSON_AddBoolToObject(req_body, "required", 1);
                cJSON* content = cJSON_AddObjectToObject(req_body, "content");
                if (content) {
                    cJSON* json_content = cJSON_AddObjectToObject(content, "application/json");
                    if (json_content) {
                        cJSON* ref_schema = cJSON_CreateObject();
                        char   ref[256];
                        snprintf(ref, sizeof(ref), "#/components/schemas/%s", input_type);
                        cJSON_AddStringToObject(ref_schema, "$ref", ref);
                        cJSON_AddItemToObject(json_content, "schema", ref_schema);
                    }
                }
            }
        }

        // Responses
        cJSON* responses = cJSON_AddObjectToObject(operation, "responses");
        if (responses) {
            const char* output_type = output_item ? cJSON_GetStringValue(output_item) : nullptr;
            int         has_output = (output_type && *output_type != '\0');

            if (has_output && schemas) {
                add_schema(schemas, output_type);
            }

            // Default 200 response
            cJSON* resp200 = cJSON_AddObjectToObject(responses, "200");
            if (resp200) {
                cJSON_AddStringToObject(resp200, "description", "Success");
                if (has_output) {
                    cJSON* content = cJSON_AddObjectToObject(resp200, "content");
                    if (content) {
                        cJSON* json_content = cJSON_AddObjectToObject(content, "application/json");
                        if (json_content) {
                            cJSON* ref_schema = cJSON_CreateObject();
                            char   ref[256];
                            snprintf(ref,
                                     sizeof(ref),
                                     "#/components/"
                                     "schemas/%s",
                                     output_type);
                            cJSON_AddStringToObject(ref_schema, "$ref", ref);
                            cJSON_AddItemToObject(json_content, "schema", ref_schema);
                        }
                    }
                }
            }

            cJSON* resp400 = cJSON_AddObjectToObject(responses, "400");
            if (resp400) {
                cJSON_AddStringToObject(resp400, "description", "Bad Request");
            }

            cJSON* resp500 = cJSON_AddObjectToObject(responses, "500");
            if (resp500) {
                cJSON_AddStringToObject(resp500, "description", "Internal Server Error");
            }
        }
    }

    /*
   * Phase 2 — Orphan type registration: scan every registered reflection
   * type and add it to components/schemas if not already present.  This
   * catches types that are never directly referenced by any route's
   * input_type or output_type but are needed as nested/sub-types (e.g.,
   * a shared Address struct referenced by both User and Order schemas).
   *
   * add_schema() skips duplicates internally (cycle detection), so types
   * already registered during Phase 1 are safe no-ops.
   */
    if (schemas) {
        csilk_reflect_foreach(auto_register_schema, schemas);
    }

    cJSON_Delete(routes);
    return doc;
}

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
        cJSON* doc = csilk_generate_openapi_json(r, title, version, description);
        if (doc) {
            g_openapi_cache_json = cJSON_PrintUnformatted(doc);
            cJSON_Delete(doc);
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
