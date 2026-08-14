/**
 * @file router.c
 * @brief Router implementation: node management, route registration, and public API.
 *
 * SIMD segment extraction and trie matching live in router_match.c.
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "router_internal.h"
#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "csilk/core/internal.h"

/**
 * @brief Allocate and initialize a router trie node.
 * @param[in] segment Node segment string (validated non-NULL); copied.
 * @param[in] type    Node type (static/param/wildcard).
 * @return A newly allocated node, or NULL on allocation/strdup failure.
 */
csilk_router_node_t*
node_new(const char* segment, csilk_node_type_t type)
{
    csilk_router_node_t* node = calloc(1, sizeof(csilk_router_node_t));
    if (!node) {
        CSILK_LOG_E("Router: failed to allocate memory for router node");
        return NULL;
    }
    node->segment = strdup(segment);
    if (!node->segment) {
        CSILK_LOG_E("Router: failed to duplicate segment string '%s'", segment);
        free(node);
        return NULL;
    }
    node->segment_len = strlen(node->segment);
    node->type = type;
    CSILK_LOG_T("Router: allocated new node (segment: '%s', type: %d)", segment, type);
    return node;
}

/**
 * @brief Recursively free a router node and all its resources.
 * @param[in] node Node to free (no-op if NULL).
 * @note Frees the segment, every method-handler chain, and all child nodes.
 */
void
node_free(csilk_router_node_t* node)
{
    if (!node) {
        return;
    }
    CSILK_LOG_T("Router: freeing node (segment: '%s', type: %d)", node->segment, node->type);
    free(node->segment);
    csilk_method_handler_t* mh = node->handlers;
    while (mh) {
        csilk_method_handler_t* next = mh->next;
        free(mh->method);
        free(mh->handlers);
        free(mh->path);
        free(mh);
        mh = next;
    }
    for (int i = 0; i < node->children_count; i++) {
        node_free(node->children[i]);
    }
    free(node);
}

/**
 * @brief Create a new, empty router with a root node.
 * @return A newly allocated router, or NULL on allocation failure.
 */
csilk_router_t*
csilk_router_new()
{
    csilk_router_t* r = malloc(sizeof(csilk_router_t));
    if (!r) {
        return NULL;
    }
    r->root = node_new("", CSILK_NODE_STATIC);
    return r;
}

/**
 * @brief Free a router and its entire trie.
 * @param[in] r Router to free (no-op if NULL).
 */
void
csilk_router_free(csilk_router_t* r)
{
    if (!r) {
        return;
    }
    node_free(r->root);
    free(r);
}

/**
 * @brief Recursively collect all registered routes into a JSON array.
 * @param[in] node  Current trie node to walk.
 * @param[in] routes JSON array that each route object is appended to.
 * @note For every method-handler at the node, appends an object with method,
 *       path, input/output types, summary, and description; then recurses into
 *       children.
 */
static void
node_collect_routes(csilk_router_node_t* node, csilk_json_t* routes)
{
    if (!node) {
        return;
    }

    csilk_method_handler_t* mh = node->handlers;
    while (mh) {
        csilk_json_t* route = csilk_json_object();
        if (route) {
            csilk_json_add_string(route, "method", mh->method);
            csilk_json_add_string(route, "path", mh->path ? mh->path : "");
            csilk_json_add_string(route, "input_type", mh->input_type ? mh->input_type : "");
            csilk_json_add_string(route, "output_type", mh->output_type ? mh->output_type : "");
            csilk_json_add_string(route, "summary", mh->summary ? mh->summary : "");
            csilk_json_add_string(route, "description", mh->description ? mh->description : "");
            csilk_json_array_append(routes, route);
        }
        mh = mh->next;
    }

    for (int i = 0; i < node->children_count; i++) {
        node_collect_routes(node->children[i], routes);
    }
}

/**
 * @brief Collect all routes of a router as a JSON array.
 * @param[in] r Router to inspect (validated non-NULL with a root).
 * @return A newly allocated JSON array of route objects, or NULL on error.
 */
csilk_json_t*
csilk_router_collect_routes(csilk_router_t* r)
{
    if (!r || !r->root) {
        return NULL;
    }
    csilk_json_t* array = csilk_json_array();
    if (!array) {
        return NULL;
    }
    node_collect_routes(r->root, array);
    return array;
}

/**
 * @brief Register a route with a method, path, and full handler metadata.
 * @param[in] r              Router to register on (validated non-NULL).
 * @param[in] method         HTTP method string (validated non-NULL).
 * @param[in] path           Route path with ':' params and '*' wildcards (validated non-NULL).
 * @param[in] handlers       NULL-terminated handler array (validated non-NULL).
 * @param[in] handler_count  Number of handlers in the array.
 * @param[in] path_pattern   Pattern string recorded for the route (may be NULL).
 * @param[in] input_type     OpenAPI input type (may be NULL).
 * @param[in] output_type    OpenAPI output type (may be NULL).
 * @param[in] summary        OpenAPI summary (may be NULL).
 * @param[in] description    OpenAPI description (may be NULL).
 * @param[in] perm_required  Required permission string (may be NULL).
 * @param[in] perm_resource  Permission resource string (may be NULL).
 * @return 0 on success, -1 on invalid args or allocation failure, 0 (ignored)
 *         if an identical method+path is already registered.
 * @note Walks/creates trie nodes per path segment (static/param/wildcard),
 *       appends a method-handler entry, and stops at a wildcard segment.
 */
int
router_add_full(csilk_router_t*  r,
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
                const char*      perm_resource)
{
    if (!r || !r->root || !method || !path || !handlers) {
        return -1;
    }
    csilk_router_node_t* curr = r->root;
    const char*          p = path;
    const char*          seg;
    size_t               len;

    while ((seg = get_next_segment(&p, &len)) != NULL) {
        csilk_node_type_t type = CSILK_NODE_STATIC;
        const char*       seg_name_start = seg;
        size_t            seg_name_len = len;

        if (seg[0] == ':') {
            type = CSILK_NODE_PARAM;
            seg_name_start = seg + 1;
            seg_name_len = len - 1;
        } else if (seg[0] == '*') {
            type = CSILK_NODE_WILDCARD;
            seg_name_start = seg + 1;
            seg_name_len = len - 1;
        }

        char* seg_name = malloc(seg_name_len + 1);
        if (!seg_name) {
            CSILK_LOG_E("Router: failed to allocate memory for segment name '%.*s'",
                        (int)seg_name_len,
                        seg_name_start);
            return -1;
        }
        memcpy(seg_name, seg_name_start, seg_name_len);
        seg_name[seg_name_len] = '\0';

        csilk_router_node_t* found = NULL;
        int                  insert_pos = curr->children_count;

        for (int i = 0; i < curr->children_count; i++) {
            if (curr->children[i]->type == type &&
                strcmp(curr->children[i]->segment, seg_name) == 0) {
                found = curr->children[i];
                break;
            }
            if (found == NULL && curr->children[i]->type > type) {
                insert_pos = i;
            }
        }

        if (!found) {
            if (curr->children_count < CSILK_MAX_CHILDREN) {
                found = node_new(seg_name, type);
                if (found) {
                    for (int i = curr->children_count; i > insert_pos; i--) {
                        curr->children[i] = curr->children[i - 1];
                    }
                    curr->children[insert_pos] = found;
                    curr->children_count++;
                    CSILK_LOG_D("Router: inserted new node '%s' (type: %d) at "
                                "index %d under node '%s'",
                                seg_name,
                                type,
                                insert_pos,
                                curr->segment[0] ? curr->segment : "/");
                } else {
                    CSILK_LOG_E("Router: failed to allocate new route node for segment "
                                "'%s' in path '%s'",
                                seg_name,
                                path);
                }
            } else {
                CSILK_LOG_E("Router: failed to insert route segment '%s' in path '%s': "
                            "maximum node children limit (%d) exceeded",
                            seg_name,
                            path,
                            CSILK_MAX_CHILDREN);
            }
        } else {
            CSILK_LOG_T("Router: matched existing node '%s' (type: %d) under node '%s'",
                        seg_name,
                        type,
                        curr->segment[0] ? curr->segment : "/");
        }
        free(seg_name);
        if (!found) {
            return -1;
        }
        curr = found;
        if (type == CSILK_NODE_WILDCARD) {
            CSILK_LOG_T("Router: stopping segment processing at wildcard node '%s'", curr->segment);
            break;
        }
    }

    csilk_method_handler_t* mh = curr->handlers;
    while (mh) {
        if (strcmp(mh->method, method) == 0) {
            CSILK_LOG_W("Router: duplicate route registration ignored: %s %s", method, path);
            return 0;
        }
        mh = mh->next;
    }

    mh = malloc(sizeof(csilk_method_handler_t));
    if (mh) {
        mh->method = strdup(method);
        if (!mh->method) {
            CSILK_LOG_E("Router: failed to duplicate method string for route: %s %s", method, path);
            free(mh);
            return -1;
        }
        mh->handlers = malloc(sizeof(csilk_handler_t) * (handler_count + 1));
        if (!mh->handlers) {
            CSILK_LOG_E("Router: failed to allocate handler array for route: %s %s", method, path);
            free(mh->method);
            free(mh);
            return -1;
        }
        memcpy(mh->handlers, handlers, sizeof(csilk_handler_t) * handler_count);
        mh->handlers[handler_count] = NULL;
        mh->handler_count = handler_count;
        mh->path = path_pattern ? strdup(path_pattern) : NULL;

        if (path_pattern && !mh->path) {
            CSILK_LOG_E("Router: failed to duplicate path pattern for route: %s %s", method, path);
            free(mh->method);
            free(mh->handlers);
            free(mh);
            return -1;
        }
        mh->input_type = input_type;
        mh->output_type = output_type;
        mh->summary = summary;
        mh->description = description;
        mh->perm_required = perm_required;
        mh->perm_resource = perm_resource;
        mh->next = curr->handlers;
        curr->handlers = mh;
        CSILK_LOG_D("Router: route successfully registered: %s %s", method, path);
        return 0;
    } else {
        CSILK_LOG_E("Router: failed to allocate method handler for route: %s %s", method, path);
        return -1;
    }
}

/**
 * @brief Register a route with metadata but no permission requirement.
 * @param[in] r              Router to register on.
 * @param[in] method         HTTP method string.
 * @param[in] path           Route path.
 * @param[in] handlers       NULL-terminated handler array.
 * @param[in] handler_count  Number of handlers.
 * @param[in] path_pattern   Pattern string recorded for the route (may be NULL).
 * @param[in] input_type     OpenAPI input type (may be NULL).
 * @param[in] output_type    OpenAPI output type (may be NULL).
 * @param[in] summary        OpenAPI summary (may be NULL).
 * @param[in] description    OpenAPI description (may be NULL).
 * @return 0 on success, -1 on failure.
 * @note Thin wrapper over router_add_full with perm fields set to NULL.
 */
int
csilk_router_add_extended(csilk_router_t*  r,
                          const char*      method,
                          const char*      path,
                          csilk_handler_t* handlers,
                          size_t           handler_count,
                          const char*      path_pattern,
                          const char*      input_type,
                          const char*      output_type,
                          const char*      summary,
                          const char*      description)
{
    return router_add_full(r,
                           method,
                           path,
                           handlers,
                           handler_count,
                           path_pattern,
                           input_type,
                           output_type,
                           summary,
                           description,
                           NULL,
                           NULL);
}

/**
 * @brief Register a simple route (method + path + handlers).
 * @param[in] r              Router to register on.
 * @param[in] method         HTTP method string.
 * @param[in] path           Route path (also used as the pattern).
 * @param[in] handlers       NULL-terminated handler array.
 * @param[in] handler_count  Number of handlers.
 * @return 0 on success, -1 on failure.
 * @note Thin wrapper over csilk_router_add_extended with no metadata.
 */
int
csilk_router_add(csilk_router_t*  r,
                 const char*      method,
                 const char*      path,
                 csilk_handler_t* handlers,
                 size_t           handler_count)
{
    return csilk_router_add_extended(
        r, method, path, handlers, handler_count, path, NULL, NULL, NULL, NULL);
}

/**
 * @brief Register a route that requires a permission.
 * @param[in] r              Router to register on.
 * @param[in] method         HTTP method string.
 * @param[in] path           Route path (also used as the pattern).
 * @param[in] handlers       NULL-terminated handler array.
 * @param[in] handler_count  Number of handlers.
 * @param[in] perm_required  Required permission string.
 * @param[in] perm_resource  Permission resource string.
 * @return 0 on success, -1 on failure.
 * @note Thin wrapper over router_add_full carrying permission fields.
 */
int
csilk_router_add_perm(csilk_router_t*  r,
                      const char*      method,
                      const char*      path,
                      csilk_handler_t* handlers,
                      size_t           handler_count,
                      const char*      perm_required,
                      const char*      perm_resource)
{
    return router_add_full(r,
                           method,
                           path,
                           handlers,
                           handler_count,
                           path,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           perm_required,
                           perm_resource);
}

/**
 * @brief Register a fully-described route that also requires a permission.
 * @param[in] r              Router to register on.
 * @param[in] method         HTTP method string.
 * @param[in] path           Route path.
 * @param[in] handlers       NULL-terminated handler array.
 * @param[in] handler_count  Number of handlers.
 * @param[in] path_pattern   Pattern string recorded for the route (may be NULL).
 * @param[in] input_type     OpenAPI input type (may be NULL).
 * @param[in] output_type    OpenAPI output type (may be NULL).
 * @param[in] summary        OpenAPI summary (may be NULL).
 * @param[in] description    OpenAPI description (may be NULL).
 * @param[in] perm_required  Required permission string.
 * @param[in] perm_resource  Permission resource string.
 * @return 0 on success, -1 on failure.
 * @note Full wrapper over router_add_full including permission fields.
 */
int
csilk_router_add_extended_perm(csilk_router_t*  r,
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
                               const char*      perm_resource)
{
    return router_add_full(r,
                           method,
                           path,
                           handlers,
                           handler_count,
                           path_pattern,
                           input_type,
                           output_type,
                           summary,
                           description,
                           perm_required,
                           perm_resource);
}

/**
 * @brief Match a method+path against the router trie, returning handlers.
 * @param[in] r      Router to query (validated non-NULL with a root).
 * @param[in] method HTTP method string (validated non-NULL).
 * @param[in] path   Request path (validated non-NULL).
 * @return The matched handler array, or NULL if no route matches.
 * @note Does not capture path parameters into a context.
 */
csilk_handler_t*
csilk_router_match(const csilk_router_t* r, const char* method, const char* path)
{
    if (!r || !r->root || !method || !path) {
        return NULL;
    }
    return match_node(r->root, method, path, NULL, NULL);
}

/**
 * @brief Match a request context's method+path against the router.
 * @param[in] r Router to query (validated non-NULL with a root).
 * @param[in] c Request context providing method/path; on match its handlers,
 *             handler_index, current_handler, and params are populated.
 * @return 1 if a route matched, 0 otherwise or on invalid arguments.
 * @note Resets c->params_count, performs trie matching (capturing path params
 *       into c), and stores the resolved handler chain and method handler.
 */
int
csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c)
{
    if (!r || !c || !r->root || !c->request.method || !c->request.path) {
        CSILK_LOG_W("Invalid match parameters: router=%p, ctx=%p", (void*)r, (void*)c);
        return 0;
    }
    c->params_count = 0;
    csilk_method_handler_t* mh = NULL;
    CSILK_LOG_T("Attempting to match route for request: %s %s", c->request.method, c->request.path);
    csilk_handler_t* handlers = match_node(r->root, c->request.method, c->request.path, c, &mh);
    if (handlers) {
        c->handlers = handlers;
        c->handler_count = mh ? mh->handler_count : 0;
        c->handler_index = -1;
        c->current_handler = mh;

        CSILK_LOG_D("Route successfully matched: %s %s (pattern: %s)",
                    c->request.method,
                    c->request.path,
                    (mh && mh->path) ? mh->path : "unknown");
        return 1;
    }
    CSILK_LOG_D("Route not matched: %s %s", c->request.method, c->request.path);
    return 0;
}
