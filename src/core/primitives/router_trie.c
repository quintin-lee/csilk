/**
 * @file router_trie.c
 * @brief Trie-based route matching and path parameter extraction.
 *
 * Traverses the router trie to match incoming request paths against
 * registered routes, extracting path parameters along the way.
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "router_internal.h"

/**
 * @brief Try to match a static child node against the current URL segment.
 * @param[in] child    Candidate static child node.
 * @param[in] method   HTTP method being routed.
 * @param[in] seg      Current URL segment (not NUL-terminated).
 * @param[in] len      Length of seg.
 * @param[in] p        Remaining path after seg.
 * @param[in] ctx      Request context (for logging/param capture).
 * @param[out] out_mh  Receives the matched method handler (may be NULL).
 * @return The matched handler, or NULL if the segment does not equal the child's
 *         static segment (using SIMD or strncmp per use_simd) or the deeper
 *         recursion failed.
 */
static csilk_handler_t*
try_match_static(csilk_router_node_t*     child,
                 const char*              method,
                 const char*              seg,
                 size_t                   len,
                 const char*              p,
                 csilk_ctx_t*             ctx,
                 csilk_method_handler_t** out_mh,
                 int                      use_simd)
{
    if (child->segment_len == len && child->segment[0] == seg[0]) {
        int match = 0;
        if (use_simd) {
            match = csilk_memcmp_fast(child->segment, seg, len);
        } else {
            match = (strncmp(child->segment, seg, len) == 0);
        }

        if (match) {
            CSILK_LOG_T("Router: STATIC child '%s' matches segment '%.*s', recursing",
                        child->segment,
                        (int)len,
                        seg);
            csilk_handler_t* r = match_node(child, method, p, ctx, out_mh);
            if (r) {
                return r;
            }
            CSILK_LOG_T("Router: backtrack - match failed deeper for STATIC child '%s'",
                        child->segment);
        }
    }
    return NULL;
}

/**
 * @brief Try to match a parameter child, capturing the segment as a path param.
 * @param[in] child    Candidate parameter child node (its segment is the key).
 * @param[in] method   HTTP method being routed.
 * @param[in] seg      Current URL segment (not NUL-terminated).
 * @param[in] len      Length of seg.
 * @param[in] p        Remaining path after seg.
 * @param[in] ctx      Request context whose params are appended to on success.
 * @param[out] out_mh  Receives the matched method handler (may be NULL).
 * @return The matched handler, or NULL if the parameter could not be captured
 *         (e.g. param limit reached) or the deeper recursion failed. On failure
 *         any captured param is rolled back.
 */
static csilk_handler_t*
try_match_param(csilk_router_node_t*     child,
                const char*              method,
                const char*              seg,
                size_t                   len,
                const char*              p,
                csilk_ctx_t*             ctx,
                csilk_method_handler_t** out_mh)
{
    int param_added = 0;
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        if (ctx->arena) {
            ctx->params[ctx->params_count].key = (char*)child->segment;
            ctx->params[ctx->params_count].value = csilk_arena_strndup(ctx->arena, seg, len);
        } else {
            ctx->params[ctx->params_count].key = strdup(child->segment);
            ctx->params[ctx->params_count].value = malloc(len + 1);
            if (ctx->params[ctx->params_count].value) {
                memcpy(ctx->params[ctx->params_count].value, seg, len);
                ctx->params[ctx->params_count].value[len] = '\0';
            }
        }

        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            param_added = 1;
            CSILK_LOG_T("Router: PARAM child '%s' matched segment "
                        "'%.*s', captured parameter",
                        child->segment,
                        (int)len,
                        seg);
        } else {
            if (!ctx->arena) {
                free(ctx->params[ctx->params_count].key);
                free(ctx->params[ctx->params_count].value);
            }
            CSILK_LOG_E("Router: failed to allocate path parameter "
                        "memory for key '%s'",
                        child->segment);
        }
    } else if (ctx) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while "
                    "parsing key '%s'",
                    CSILK_MAX_PARAMS,
                    child->segment);
    }

    csilk_handler_t* r = match_node(child, method, p, ctx, out_mh);

    if (!r && param_added) {
        CSILK_LOG_T("Router: backtrack - match failed deeper for PARAM "
                    "child '%s', rolling back parameter",
                    child->segment);
        ctx->params_count--;
        if (!ctx->arena) {
            free(ctx->params[ctx->params_count].key);
            free(ctx->params[ctx->params_count].value);
        }
    }
    return r;
}

/**
 * @brief Try to match a wildcard child, capturing the rest of the path.
 * @param[in] child    Candidate wildcard child node (its segment is the key).
 * @param[in] method   HTTP method being routed.
 * @param[in] path     Remaining request path (may be NULL).
 * @param[in] ctx      Request context whose params are appended to on success.
 * @param[out] out_mh  Receives the matched method handler (may be NULL).
 * @return The matched handler (from the child's method list), or NULL if the
 *         parameter limit is reached or no method matches.
 * @note Captures the leading-slash-stripped remainder of path as the wildcard
 *       parameter value.
 */
static csilk_handler_t*
try_match_wildcard(csilk_router_node_t*     child,
                   const char*              method,
                   const char*              path,
                   csilk_ctx_t*             ctx,
                   csilk_method_handler_t** out_mh)
{
    CSILK_LOG_T("Router: WILDCARD child '%s' matches remaining path '%s'", child->segment, path);
    int param_added = 0;
    if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        const char* val_start = path;
        while (*val_start == '/') {
            val_start++;
        }

        if (ctx->arena) {
            ctx->params[ctx->params_count].key = csilk_arena_strdup(ctx->arena, child->segment);
            ctx->params[ctx->params_count].value = csilk_arena_strdup(ctx->arena, val_start);
        } else {
            ctx->params[ctx->params_count].key = strdup(child->segment);
            ctx->params[ctx->params_count].value = strdup(val_start);
        }

        if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
            ctx->params_count++;
            param_added = 1;
            CSILK_LOG_T(
                "Router: captured wildcard parameter '%s' = '%s'", child->segment, val_start);
        } else {
            if (!ctx->arena) {
                free(ctx->params[ctx->params_count].key);
                free(ctx->params[ctx->params_count].value);
            }
            CSILK_LOG_E("Router: failed to allocate wildcard path parameter "
                        "memory for key '%s'",
                        child->segment);
        }
    } else if (ctx) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while parsing "
                    "wildcard key '%s'",
                    CSILK_MAX_PARAMS,
                    child->segment);
    }

    csilk_method_handler_t* mh = child->handlers;
    while (mh) {
        if (strcmp(mh->method, method) == 0) {
            if (out_mh) {
                *out_mh = mh;
            }
            CSILK_LOG_T("Router: matched handler for method '%s' at "
                        "wildcard node '%s'",
                        method,
                        child->segment);
            return mh->handlers;
        }
        mh = mh->next;
    }

    if (param_added) {
        CSILK_LOG_T("Router: backtrack - method '%s' mismatch for WILDCARD "
                    "child '%s', rolling back parameter",
                    method,
                    child->segment);
        ctx->params_count--;
        if (!ctx->arena) {
            free(ctx->params[ctx->params_count].key);
            free(ctx->params[ctx->params_count].value);
        }
    }

    return NULL;
}

/**
 * @brief Recursively match a request path against the router trie.
 * @param[in] node    Current trie node being visited.
 * @param[in] method  HTTP method being routed.
 * @param[in] path    Remaining request path (NULL/empty means terminal).
 * @param[in] ctx     Request context (for SIMD config, param capture, logging).
 * @param[out] out_mh Receives the matched method handler (may be NULL).
 * @return The handler for the matching route, or NULL if no route matches.
 * @note At a terminal node returns the handler whose method matches; otherwise
 *       splits off the next segment and tries static, parameter, and wildcard
 *       children in order.
 */
csilk_handler_t*
match_node(csilk_router_node_t*     node,
           const char*              method,
           const char*              path,
           csilk_ctx_t*             ctx,
           csilk_method_handler_t** out_mh)
{
    int use_simd = (ctx && ctx->server) ? ctx->server->config.enable_simd : 1;

    CSILK_LOG_T("Router: matching node '%s' (type: %d) with remaining path '%s'",
                node->segment[0] ? node->segment : "/",
                node->type,
                path ? path : "empty");

    if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
        CSILK_LOG_T("Router: reached leaf/terminal match at node '%s'",
                    node->segment[0] ? node->segment : "/");
        csilk_method_handler_t* mh = node->handlers;
        while (mh) {
            if (strcmp(mh->method, method) == 0) {
                if (out_mh) {
                    *out_mh = mh;
                }
                CSILK_LOG_T("Router: matched handler for method '%s' at node '%s'",
                            method,
                            node->segment[0] ? node->segment : "/");
                return mh->handlers;
            }
            mh = mh->next;
        }
        CSILK_LOG_T("Router: no handler for method '%s' at node '%s'",
                    method,
                    node->segment[0] ? node->segment : "/");
        return NULL;
    }

    const char* p = path;
    size_t      len;
    const char* seg = get_next_segment(&p, &len);
    if (!seg) {
        CSILK_LOG_T("Router: no more segments found in path '%s'", path);
        return NULL;
    }

    CSILK_LOG_T("Router: testing segment '%.*s' against %d children of node '%s'",
                (int)len,
                seg,
                node->children_count,
                node->segment[0] ? node->segment : "/");

    csilk_handler_t*      result = NULL;
    csilk_router_node_t** children = node_children(node);
    for (int i = 0; i < node->children_count; i++) {
        csilk_router_node_t* child = children[i];
        if (child->type == CSILK_NODE_STATIC) {
            result = try_match_static(child, method, seg, len, p, ctx, out_mh, use_simd);
        } else if (child->type == CSILK_NODE_PARAM) {
            result = try_match_param(child, method, seg, len, p, ctx, out_mh);
        } else if (child->type == CSILK_NODE_WILDCARD) {
            result = try_match_wildcard(child, method, path, ctx, out_mh);
        }
        if (result) {
            break;
        }
    }

    return result;
}
