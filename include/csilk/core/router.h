#pragma once
/**
 * @file router.h
 * @brief High-performance HTTP router based on a compressed radix tree.
 *
 * Supports dynamic path segments (:param), wildcards (*wildcard),
 * route metadata for OpenAPI generation, and permission annotations.
 *
 * @version 0.3.0
 * @copyright MIT License
 */

#include "csilk/core/types.h"
#include "csilk/core/context.h"

/**
 * @brief The main HTTP router.
 *
 * Wraps a radix-tree root node and provides methods to register routes,
 * match incoming requests, and generate OpenAPI specs.
 *
 * @note Not thread-safe for mutation after the server starts.  All routes
 *       must be registered before csilk_server_run.
 */
typedef struct csilk_router_s {
    csilk_router_node_t* root; /**< Root node of the radix (Patricia) trie. */
} csilk_router_t;

/**
 * @brief Create a new empty router.
 *
 * Allocates and initialises the router structure with a single root node.
 *
 * @return A pointer to the new router (heap-allocated), or nullptr on allocation
 *         failure.
 */
csilk_router_t* csilk_router_new(void);

/**
 * @brief Register a route with one or more handlers.
 *
 * The route is inserted into the radix tree.  Dynamic segments (:param) and
 * wildcard segments (*wildcard) are supported in @p path.  The handlers are
 * stored by pointer — the caller must ensure they remain valid for the
 * lifetime of the router.
 *
 * @param r             Router instance.
 * @param method        HTTP method (e.g., "GET", "POST", "DELETE", "*" for
 * any).
 * @param path          URL pattern (e.g., "/users/:id/posts").
 * @param handlers      Array of handler function pointers.
 * @param handler_count Number of elements in @p handlers.
 */
int csilk_router_add(csilk_router_t*  r,
                     const char*      method,
                     const char*      path,
                     csilk_handler_t* handlers,
                     size_t           handler_count);

/**
 * @brief Match a raw method+path to handlers (standalone, no context).
 *
 * Useful for testing or when no csilk_ctx_t is available.  The returned
 * array is owned by the router and must NOT be freed.
 *
 * @param r      Router instance.
 * @param method HTTP method string.
 * @param path   Decoded URL path.
 * @return Pointer to the handler array for the matched route, or nullptr if
 *         no route matches.
 */
csilk_handler_t* csilk_router_match(const csilk_router_t* r, const char* method, const char* path);

/**
 * @brief Match the current request against the router and update the context.
 *
 * On success the matched handlers are stored in the context and path
 * parameters (csilk_get_param) become available.
 *
 * @param r  Router instance.
 * @param c  Request context containing the parsed request.
 * @return 1 if a matching route was found, 0 if no route matches.
 */
int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c);

/**
 * @brief Destroy the router and release all its resources.
 *
 * Frees the radix tree nodes and any associated copy of route metadata
 * (OpenAPI annotations).
 *
 * @param r  Router instance to free.  Must not be nullptr.
 */
void csilk_router_free(csilk_router_t* r);

/**
 * @brief Collect metadata for all registered routes.
 *
 * Traverses the radix tree and returns a cJSON array where each element
 * contains "method", "path", "input_type", "output_type", "summary", and
 * "description" fields.
 *
 * @param r  Router instance.
 * @return A cJSON array (caller must free with cJSON_Delete), or nullptr on
 *         allocation failure.
 */
cJSON* csilk_router_collect_routes(csilk_router_t* r);

/**
 * @brief Generate an OpenAPI 3.0 specification JSON from the router.
 *
 * Traverses all registered routes and uses the reflection system to build
 * JSON schemas for request bodies and responses.  Produces a complete
 * OpenAPI document suitable for use with Swagger UI, Redoc, etc.
 *
 * @param router      The router instance.
 * @param title       API title for the OpenAPI info block.
 * @param version     API version for the OpenAPI info block.
 * @param description API description (optional — pass nullptr to omit).
 * @return A cJSON object representing the full OpenAPI spec.  Caller must
 *         free with cJSON_Delete.
 */
cJSON* csilk_generate_openapi_json(csilk_router_t* router,
                                   const char*     title,
                                   const char*     version,
                                   const char*     description);

/**
 * @brief Register a route with full OpenAPI/reflection metadata.
 *
 * Extended version of csilk_router_add that also stores metadata for
 * automatic OpenAPI spec generation and request/response binding.
 *
 * @param r             Router instance.
 * @param method        HTTP method string.
 * @param path          URL pattern (e.g., "/users/:id").
 * @param handlers      Array of handler functions.
 * @param handler_count Number of handlers in @p handlers.
 * @param path_pattern  Canonical path pattern string for documentation
 *                      (may differ from the radix-tree path).
 * @param input_type    Registered type name for request-body binding
 *                      (nullptr if there is no request body).
 * @param output_type   Registered type name for response serialisation
 *                      (nullptr if raw response is used).
 * @param summary       Short summary of the operation (nullptr to omit from spec).
 * @param description   Detailed description of the operation (nullptr to omit).
 */
int csilk_router_add_extended(csilk_router_t*  r,
                              const char*      method,
                              const char*      path,
                              csilk_handler_t* handlers,
                              size_t           handler_count,
                              const char*      path_pattern,
                              const char*      input_type,
                              const char*      output_type,
                              const char*      summary,
                              const char*      description);

/** @brief Register a route with permission metadata.
 *  @param r             Router instance.
 *  @param method        HTTP method string.
 *  @param path          URL pattern.
 *  @param handlers      Handler function array.
 *  @param handler_count Number of handlers.
 *  @param perm_required Permission identifier (e.g., "read"), or nullptr.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
int csilk_router_add_perm(csilk_router_t*  r,
                          const char*      method,
                          const char*      path,
                          csilk_handler_t* handlers,
                          size_t           handler_count,
                          const char*      perm_required,
                          const char*      perm_resource);

/** @brief Register a route with full metadata including permissions.
 *  @param r             Router instance.
 *  @param method        HTTP method string.
 *  @param path          URL pattern.
 *  @param handlers      Handler function array.
 *  @param handler_count Number of handlers.
 *  @param path_pattern  Canonical path pattern for docs.
 *  @param input_type    Registered type name for request-body (nullptr if none).
 *  @param output_type   Registered type name for response (nullptr if none).
 *  @param summary       Short operation summary (nullptr to omit).
 *  @param description   Detailed description (nullptr to omit).
 *  @param perm_required Permission identifier (e.g., "read"), or nullptr.
 *  @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
int csilk_router_add_extended_perm(csilk_router_t*  r,
                                   const char*      method,
                                   const char*      path,
                                   csilk_handler_t* handlers,
                                   size_t           handler_count,
                                   const char*      path_pattern,
                                   const char*      input_type,
                                   const char*      output_type,
                                   const char*      summary,
                                   const char*      description,
                                   const char*      perm_required,
                                   const char*      perm_resource);

/**
 * @brief Metadata for a route used to generate OpenAPI documentation.
 */
typedef struct {
    const char* input_type;  /**< Request body type name. */
    const char* output_type; /**< Response type name. */
    const char* summary;     /**< Short summary. */
    const char* description; /**< Detailed description. */
} csilk_route_metadata_t;

/**
 * @brief Convenience macro to register a route with metadata.
 *
 * Automatically passes @p path as both the URL pattern and the documentation
 * path pattern.  Wraps csilk_router_add_extended.
 */
#define CSILK_ROUTE(                                                                               \
    r, method, path, handlers, handler_count, input_type, output_type, summary, desc)              \
    csilk_router_add_extended(                                                                     \
        r, method, path, handlers, handler_count, path, input_type, output_type, summary, desc)

/**
 * @brief Register a route with a metadata struct for OpenAPI documentation.
 */
#define CSILK_REGISTER_ROUTE_DOC(r, method, path, handlers, handler_count, meta)                   \
    csilk_router_add_extended(r,                                                                   \
                              method,                                                              \
                              path,                                                                \
                              handlers,                                                            \
                              handler_count,                                                       \
                              path,                                                                \
                              (meta).input_type,                                                   \
                              (meta).output_type,                                                  \
                              (meta).summary,                                                      \
                              (meta).description)

/**
 * @brief Serve the OpenAPI JSON specification as the response.
 *
 * Intended to be called from within a handler to expose the API spec.
 *
 * @code
 * void openapi_handler(csilk_ctx_t* c) {
 *     csilk_serve_openapi(c, router, "My API", "1.0.0", "API Description");
 * }
 * @endcode
 *
 * @param c           The request context.
 * @param r           The router instance whose routes will be documented.
 * @param title       API title.
 * @param version     API version.
 * @param description API description (optional, pass nullptr to omit).
 */
void csilk_serve_openapi(csilk_ctx_t*    c,
                         csilk_router_t* r,
                         const char*     title,
                         const char*     version,
                         const char*     description);

/**
 * @brief Serve the embedded Swagger UI page.
 *
 * The UI loads the OpenAPI spec from /openapi.json (the client fetches it
 * separately).  Register a handler for GET /openapi.json that calls
 * csilk_serve_openapi.
 *
 * @code
 * void docs_handler(csilk_ctx_t* c) {
 *     csilk_serve_swagger_ui(c);
 * }
 * @endcode
 *
 * @param c  The request context.
 */
void csilk_serve_swagger_ui(csilk_ctx_t* c);
