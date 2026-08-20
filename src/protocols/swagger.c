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
 * @return A csilk_json_t object representing the OpenAPI schema, or NULL if the
 *         type is not registered or allocation fails.
 * @note The caller must free the returned csilk_json_t with csilk_json_free(). */
static csilk_json_t*
generate_schema_for_type(const char* type_name)
{
    const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
    if (!entry) {
        return NULL;
    }

    csilk_json_t* schema = csilk_json_object();
    if (!schema) {
        return NULL;
    }

    csilk_json_add_string(schema, "type", "object");
    csilk_json_t* properties = csilk_json_object();
    if (!properties) {
        csilk_json_free(schema);
        return NULL;
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
        csilk_json_t*             prop = csilk_json_object();

        const char* oa_type = field_type_to_openapi_type(field->type);
        if (field->type == CSILK_TYPE_STRUCT) {
            const char* type_name = field->nested_type_name ? field->nested_type_name : "unknown";
            char        ref[256];
            snprintf(ref, sizeof(ref), "#/components/schemas/%s", type_name);
            csilk_json_add_string(prop, "$ref", ref);
        } else {
            csilk_json_add_string(prop, "type", oa_type);
        }

        if (field->array_length > 0) {
            csilk_json_t* arr_wrap = csilk_json_object();
            csilk_json_add_object(arr_wrap, "items", prop);
            csilk_json_add_string(arr_wrap, "type", "array");
            csilk_json_add_object(properties, field->json_key, arr_wrap);
        } else {
            csilk_json_add_object(properties, field->json_key, prop);
        }
    }

    csilk_json_add_object(schema, "properties", properties);
    return schema;
}

/** @brief Internal: add a schema definition to the components/schemas map.
 *
 * If the schema for @p type_name already exists in @p schemas, this is a
 * no-op (prevents infinite recursion on circular type references). Otherwise
 * generates the schema, adds it, and recursively registers schemas for any
 * nested struct fields.
 *
 * @param schemas   The "schemas" csilk_json_t object under components.
 * @param type_name Type name to generate and add. */
static void
add_schema(csilk_json_t* schemas, const char* type_name)
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
   * The schemas csilk_json_t object doubles as a "visited" set: we insert the
   * schema upfront (cJSON_AddItemToObject) before recursing into nested
   * types, so a re-encounter is caught immediately.  The "$ref" pointer
   * in the parent schema correctly references the already-inserted entry.
   */
    if (csilk_json_get(schemas, type_name)) {
        return;
    }

    csilk_json_t* schema = generate_schema_for_type(type_name);
    if (schema) {
        csilk_json_add_object(schemas, type_name, schema);

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
 * @param user_data Pointer to the schemas csilk_json_t object. */
static void
auto_register_schema(const char* name, const csilk_reflect_entry_t* entry, void* user_data)
{
    (void)entry;
    add_schema((csilk_json_t*)user_data, name);
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
 * @param title       API title for the info section (pass NULL for default).
 * @param version     API version string (pass NULL for default "1.0.0").
 * @param description API description (may be NULL).
 * @return A csilk_json_t object representing the full OpenAPI document. Caller must
 *         free with csilk_json_free(). Returns NULL if router is NULL or
 *         allocation fails. */
csilk_json_t*
csilk_generate_openapi_json(csilk_router_t* router,
                            const char*     title,
                            const char*     version,
                            const char*     description)
{
    if (!router) {
        return NULL;
    }

    csilk_json_t* doc = csilk_json_object();
    if (!doc) {
        return NULL;
    }

    // OpenAPI version
    csilk_json_add_string(doc, "openapi", "3.0.3");

    // Info section
    csilk_json_t* info = csilk_json_object();
    if (info) {
        csilk_json_add_string(info, "title", title ? title : "csilk API");
        csilk_json_add_string(info, "version", version ? version : "1.0.0");
        if (description) {
            csilk_json_add_string(info, "description", description);
        }
        csilk_json_add_object(doc, "info", info);
    }

    // Paths section
    csilk_json_t* paths = csilk_json_object();

    // Components section
    csilk_json_t* components = csilk_json_object();
    csilk_json_t* schemas = NULL;
    if (components) {
        schemas = csilk_json_object();
    }

    // Collect all routes
    csilk_json_t* routes = csilk_router_collect_routes(router);
    if (!routes) {
        csilk_json_free(doc);
        return NULL;
    }

    /*
   * Phase 1 — Build the "paths" section by iterating every registered route.
   * Each route from csilk_router_collect_routes() carries method, path,
   * input_type, output_type, summary, and description as csilk_json_t properties.
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
    csilk_json_t* route;
    for (size_t _i = 0; _i < csilk_json_array_size(routes); _i++) {
        route = csilk_json_array_get(routes, _i);
        if (!route) {
            break;
        }
        {
            csilk_json_t* method_item = csilk_json_get(route, "method");
            csilk_json_t* path_item = csilk_json_get(route, "path");
            csilk_json_t* input_item = csilk_json_get(route, "input_type");
            csilk_json_t* output_item = csilk_json_get(route, "output_type");
            csilk_json_t* summary_item = csilk_json_get(route, "summary");
            csilk_json_t* desc_item = csilk_json_get(route, "description");

            if (!method_item || !path_item) {
                continue;
            }

            const char* method = csilk_json_string_value(method_item);
            const char* raw_path = csilk_json_string_value(path_item);
            if (!method || !raw_path) {
                continue;
            }

            // Convert path to OpenAPI format
            char oa_path[1024];
            path_to_openapi(raw_path, oa_path, sizeof(oa_path));

            // Get or create path item
            csilk_json_t* path_obj = csilk_json_get(paths, oa_path);
            if (!path_obj) {
                csilk_json_add_object(paths, oa_path, csilk_json_object());
                path_obj = csilk_json_get(paths, oa_path);
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
            if (csilk_json_get(path_obj, method_lower)) {
                continue;
            }

            csilk_json_add_object(path_obj, method_lower, csilk_json_object());
            csilk_json_t* operation = csilk_json_get(path_obj, method_lower);
            if (!operation) {
                continue;
            }

            // Summary and description
            csilk_json_add_string(operation,
                                  "summary",
                                  summary_item && csilk_json_string_value(summary_item)
                                      ? csilk_json_string_value(summary_item)
                                      : "");
            csilk_json_add_string(operation,
                                  "description",
                                  desc_item && csilk_json_string_value(desc_item)
                                      ? csilk_json_string_value(desc_item)
                                      : "");
            // Add Operation ID: method_path
            {
                char opid[1024];
                snprintf(opid, sizeof(opid), "%s%s", method, oa_path);
                csilk_json_add_string(operation, "operationId", opid);
            }

            // Parameters (path params extracted from path pattern)
            csilk_json_t* params =
                csilk_json_add_array_obj(operation, "parameters", csilk_json_array());

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

                    csilk_json_t* param = csilk_json_object();
                    {
                        char   param_name[128];
                        size_t clen = len < sizeof(param_name) - 1 ? len : sizeof(param_name) - 1;
                        memcpy(param_name, start, clen);
                        param_name[clen] = '\0';
                        csilk_json_add_string(param, "name", param_name);
                    }
                    csilk_json_add_string(param, "in", "path");
                    csilk_json_add_bool(param, "required", 1);
                    csilk_json_t* schema_obj = csilk_json_object();
                    csilk_json_add_string(schema_obj, "type", "string");
                    csilk_json_add_object(param, "schema", schema_obj);
                    csilk_json_array_append(params, param);
                } else if (*p == '*') {
                    p++;
                    const char* start = p;
                    while (*p) {
                        p++;
                    }
                    size_t len = (size_t)(p - start);

                    csilk_json_t* param = csilk_json_object();
                    {
                        char   param_name[128];
                        size_t clen = len < sizeof(param_name) - 1 ? len : sizeof(param_name) - 1;
                        memcpy(param_name, start, clen);
                        param_name[clen] = '\0';
                        csilk_json_add_string(param, "name", param_name);
                    }
                    csilk_json_add_string(param, "in", "path");
                    csilk_json_add_bool(param, "required", 1);
                    csilk_json_t* schema_obj = csilk_json_object();
                    csilk_json_add_string(schema_obj, "type", "string");
                    csilk_json_add_object(param, "schema", schema_obj);
                    csilk_json_array_append(params, param);
                } else {
                    p++;
                }
            }

            // Request body (if input_type is set)
            const char* input_type = input_item ? csilk_json_string_value(input_item) : NULL;
            if (input_type && *input_type != '\0') {
                // Add schema for this type
                if (schemas) {
                    add_schema(schemas, input_type);
                }

                csilk_json_t* ref_schema = csilk_json_object();
                char          ref[256];
                snprintf(ref, sizeof(ref), "#/components/schemas/%s", input_type);
                csilk_json_add_string(ref_schema, "$ref", ref);

                csilk_json_t* json_content = csilk_json_object();
                csilk_json_add_object(json_content, "schema", ref_schema);

                csilk_json_t* content = csilk_json_object();
                csilk_json_add_object(content, "application/json", json_content);

                csilk_json_t* req_body = csilk_json_object();
                csilk_json_add_bool(req_body, "required", 1);
                csilk_json_add_object(req_body, "content", content);

                csilk_json_add_object(operation, "requestBody", req_body);
            }

            // Responses
            const char* output_type = output_item ? csilk_json_string_value(output_item) : NULL;
            int         has_output = (output_type && *output_type != '\0');

            if (has_output && schemas) {
                add_schema(schemas, output_type);
            }

            csilk_json_t* responses = csilk_json_object();

            // Default 200 response
            csilk_json_t* resp200 = csilk_json_object();
            csilk_json_add_string(resp200, "description", "Success");
            if (has_output) {
                csilk_json_t* json_content = csilk_json_object();
                csilk_json_t* ref_schema = csilk_json_object();
                char          ref[256];
                snprintf(ref, sizeof(ref), "#/components/schemas/%s", output_type);
                csilk_json_add_string(ref_schema, "$ref", ref);
                csilk_json_add_object(json_content, "schema", ref_schema);

                csilk_json_t* content = csilk_json_object();
                csilk_json_add_object(content, "application/json", json_content);
                csilk_json_add_object(resp200, "content", content);
            }
            csilk_json_add_object(responses, "200", resp200);

            csilk_json_t* resp400 = csilk_json_object();
            csilk_json_add_string(resp400, "description", "Bad Request");
            csilk_json_add_object(responses, "400", resp400);

            csilk_json_t* resp500 = csilk_json_object();
            csilk_json_add_string(resp500, "description", "Internal Server Error");
            csilk_json_add_object(responses, "500", resp500);

            csilk_json_add_object(operation, "responses", responses);
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
    if (schemas && components) {
        csilk_json_add_object(components, "schemas", schemas);
    }
    if (components) {
        csilk_json_add_object(doc, "components", components);
    }
    if (paths) {
        csilk_json_add_object(doc, "paths", paths);
    }

    csilk_json_free(routes);
    return doc;
}

/**
 * @brief Serve the generated OpenAPI 3.0 spec as a JSON response (cached).
 *
 * Generates the OpenAPI document once via csilk_generate_openapi_json() and
 * caches the serialized JSON in a process-global, mutex-guarded buffer;
 * subsequent calls return the cached copy. Intended to be called from a route
 * handler. On failure a 500 error response is sent.
 *
 * @param c           The request context.
 * @param r           The router instance used to build the spec.
 * @param title       API title (may be NULL for default).
 * @param version     API version (may be NULL for default "1.0.0").
 * @param description API description (may be NULL).
 * @note The response is sent synchronously via csilk_json_string(). */
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

    csilk_json_t* doc = csilk_generate_openapi_json(r, title, version, description);
    if (doc) {
        csilk_json(c, CSILK_STATUS_OK, doc);
    } else {
        csilk_json_error(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "Failed to generate OpenAPI spec");
    }
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
