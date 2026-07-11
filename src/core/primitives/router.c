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

csilk_router_node_t*
node_new(const char* segment, csilk_node_type_t type)
{
    csilk_router_node_t* node = calloc(1, sizeof(csilk_router_node_t));
    if (!node) {
        CSILK_LOG_E("Router: failed to allocate memory for router node");
        return nullptr;
    }
    node->segment = strdup(segment);
    if (!node->segment) {
        CSILK_LOG_E("Router: failed to duplicate segment string '%s'", segment);
        free(node);
        return nullptr;
    }
    node->segment_len = strlen(node->segment);
    node->type = type;
    CSILK_LOG_T("Router: allocated new node (segment: '%s', type: %d)", segment, type);
    return node;
}

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

csilk_router_t*
csilk_router_new()
{
    csilk_router_t* r = malloc(sizeof(csilk_router_t));
    if (!r) {
        return nullptr;
    }
    r->root = node_new("", CSILK_NODE_STATIC);
    return r;
}

void
csilk_router_free(csilk_router_t* r)
{
    if (!r) {
        return;
    }
    node_free(r->root);
    free(r);
}

static void
node_collect_routes(csilk_router_node_t* node, cJSON* routes)
{
    if (!node) {
        return;
    }

    csilk_method_handler_t* mh = node->handlers;
    while (mh) {
        cJSON* route = cJSON_CreateObject();
        if (route) {
            cJSON_AddStringToObject(route, "method", mh->method);
            cJSON_AddStringToObject(route, "path", mh->path ? mh->path : "");
            cJSON_AddStringToObject(route, "input_type", mh->input_type ? mh->input_type : "");
            cJSON_AddStringToObject(route, "output_type", mh->output_type ? mh->output_type : "");
            cJSON_AddStringToObject(route, "summary", mh->summary ? mh->summary : "");
            cJSON_AddStringToObject(route, "description", mh->description ? mh->description : "");
            cJSON_AddItemToArray(routes, route);
        }
        mh = mh->next;
    }

    for (int i = 0; i < node->children_count; i++) {
        node_collect_routes(node->children[i], routes);
    }
}

cJSON*
csilk_router_collect_routes(csilk_router_t* r)
{
    if (!r || !r->root) {
        return nullptr;
    }
    cJSON* array = cJSON_CreateArray();
    if (!array) {
        return nullptr;
    }
    node_collect_routes(r->root, array);
    return array;
}

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

    while ((seg = get_next_segment(&p, &len)) != nullptr) {
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

        csilk_router_node_t* found = nullptr;
        int                  insert_pos = curr->children_count;

        for (int i = 0; i < curr->children_count; i++) {
            if (curr->children[i]->type == type &&
                strcmp(curr->children[i]->segment, seg_name) == 0) {
                found = curr->children[i];
                break;
            }
            if (found == nullptr && curr->children[i]->type > type) {
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
        mh->handlers[handler_count] = nullptr;
        mh->path = path_pattern ? strdup(path_pattern) : nullptr;
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
    router_add_full(r,
                    method,
                    path,
                    handlers,
                    handler_count,
                    path_pattern,
                    input_type,
                    output_type,
                    summary,
                    description,
                    nullptr,
                    nullptr);
}

int
csilk_router_add(csilk_router_t*  r,
                 const char*      method,
                 const char*      path,
                 csilk_handler_t* handlers,
                 size_t           handler_count)
{
    return csilk_router_add_extended(
        r, method, path, handlers, handler_count, path, nullptr, nullptr, nullptr, nullptr);
}

int
csilk_router_add_perm(csilk_router_t*  r,
                      const char*      method,
                      const char*      path,
                      csilk_handler_t* handlers,
                      size_t           handler_count,
                      const char*      perm_required,
                      const char*      perm_resource)
{
    router_add_full(r,
                    method,
                    path,
                    handlers,
                    handler_count,
                    path,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    perm_required,
                    perm_resource);
}

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
    router_add_full(r,
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

csilk_handler_t*
csilk_router_match(const csilk_router_t* r, const char* method, const char* path)
{
    if (!r || !r->root || !method || !path) {
        return nullptr;
    }
    return match_node(r->root, method, path, nullptr, nullptr);
}

int
csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c)
{
    if (!r || !c || !r->root || !c->request.method || !c->request.path) {
        CSILK_LOG_W("Invalid match parameters: router=%p, ctx=%p", (void*)r, (void*)c);
        return 0;
    }
    c->params_count = 0;
    csilk_method_handler_t* mh = nullptr;
    CSILK_LOG_T("Attempting to match route for request: %s %s", c->request.method, c->request.path);
    csilk_handler_t* handlers = match_node(r->root, c->request.method, c->request.path, c, &mh);
    if (handlers) {
        c->handlers = handlers;
        c->handler_index = -1;
        c->current_handler = mh;
        CSILK_LOG_D("Route successfully matched: %s %s (pattern: %s)",
                    c->request.method,
                    c->request.path,
                    mh->path ? mh->path : "unknown");
        return 1;
    }
    CSILK_LOG_D("Route not matched: %s %s", c->request.method, c->request.path);
    return 0;
}
