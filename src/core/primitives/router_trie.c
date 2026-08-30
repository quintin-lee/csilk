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
#include "../internal/srv_impl.h"

/**
 * @brief Stack frame for iterative non-recursive trie traversal.
 */
typedef struct {
    csilk_router_node_t* node;      /**< Current trie node */
    const char*          path;      /**< Remaining path string at this frame */
    const char*          p;         /**< Path position after consumed segment */
    const char*          seg;       /**< Extracted segment pointer */
    size_t               seg_len;   /**< Segment length */
    int                  child_idx; /**< Next child index to explore [0..children_count] */
    int                  params_count_snapshot; /**< Parameter count snapshot for rollback */
    int                  param_added;           /**< 1 if parameter was captured at this frame */
    int                  checked_terminal;      /**< 1 if terminal leaf handler check was done */
} router_stack_frame_t;

/**
 * @brief Capture a standard path parameter into context.
 */
static inline int
capture_param(csilk_ctx_t* ctx, const char* key, const char* val, size_t val_len)
{
    if (!ctx) {
        return 0;
    }
    if (ctx->params_count >= CSILK_MAX_PARAMS) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while parsing key '%s'",
                    CSILK_MAX_PARAMS,
                    key);
        return 0;
    }

    if (ctx->arena) {
        ctx->params[ctx->params_count].key = (char*)key;
        ctx->params[ctx->params_count].value = csilk_arena_strndup(ctx->arena, val, val_len);
    } else {
        ctx->params[ctx->params_count].key = strdup(key);
        ctx->params[ctx->params_count].value = malloc(val_len + 1);
        if (ctx->params[ctx->params_count].value) {
            memcpy(ctx->params[ctx->params_count].value, val, val_len);
            ctx->params[ctx->params_count].value[val_len] = '\0';
        }
    }

    if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
        ctx->params_count++;
        CSILK_LOG_T("Router: PARAM child '%s' matched segment '%.*s', captured parameter",
                    key,
                    (int)val_len,
                    val);
        return 1;
    }

    if (!ctx->arena) {
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
    }
    CSILK_LOG_E("Router: failed to allocate path parameter memory for key '%s'", key);
    return 0;
}

/**
 * @brief Capture a wildcard remainder path parameter into context.
 */
static inline int
capture_wildcard_param(csilk_ctx_t* ctx, const char* key, const char* path)
{
    if (!ctx) {
        return 0;
    }
    if (ctx->params_count >= CSILK_MAX_PARAMS) {
        CSILK_LOG_E("Router: path parameter limit (%d) exceeded while parsing wildcard key '%s'",
                    CSILK_MAX_PARAMS,
                    key);
        return 0;
    }

    const char* val_start = path ? path : "";
    while (*val_start == '/') {
        val_start++;
    }

    if (ctx->arena) {
        ctx->params[ctx->params_count].key = csilk_arena_strdup(ctx->arena, key);
        ctx->params[ctx->params_count].value = csilk_arena_strdup(ctx->arena, val_start);
    } else {
        ctx->params[ctx->params_count].key = strdup(key);
        ctx->params[ctx->params_count].value = strdup(val_start);
    }

    if (ctx->params[ctx->params_count].key && ctx->params[ctx->params_count].value) {
        ctx->params_count++;
        CSILK_LOG_T("Router: captured wildcard parameter '%s' = '%s'", key, val_start);
        return 1;
    }

    if (!ctx->arena) {
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
    }
    CSILK_LOG_E("Router: failed to allocate wildcard path parameter memory for key '%s'", key);
    return 0;
}

/**
 * @brief Rollback parameters captured after snapshot.
 */
static inline void
rollback_param(csilk_ctx_t* ctx, int target_count)
{
    if (!ctx) {
        return;
    }
    while (ctx->params_count > target_count) {
        ctx->params_count--;
        if (!ctx->arena) {
            free(ctx->params[ctx->params_count].key);
            free(ctx->params[ctx->params_count].value);
        }
    }
}

/**
 * @brief Iteratively match a request path against the router trie.
 * @param[in] node    Root trie node being visited.
 * @param[in] method  HTTP method being routed.
 * @param[in] path    Request path.
 * @param[in] ctx     Request context (for SIMD config, param capture, logging).
 * @param[out] out_mh Receives the matched method handler (may be NULL).
 * @return The handler for the matching route, or NULL if no route matches.
 * @note Performs bounded non-recursive depth-first traversal with backtrack rollback.
 */
csilk_handler_t*
match_node(csilk_router_node_t*     node,
           const char*              method,
           const char*              path,
           csilk_ctx_t*             ctx,
           csilk_method_handler_t** out_mh)
{
    if (!node || !method) {
        return NULL;
    }
    if (!path) {
        path = "";
    }

    int use_simd = (ctx && ctx->server) ? _csilk_server_get_enable_simd(ctx->server) : 1;

    router_stack_frame_t stack[CSILK_ROUTER_MAX_DEPTH];
    int                  depth = 0;

    stack[0].node = node;
    stack[0].path = path;
    stack[0].child_idx = 0;
    stack[0].params_count_snapshot = ctx ? ctx->params_count : 0;
    stack[0].param_added = 0;
    stack[0].checked_terminal = 0;

    const char* p = path;
    size_t      len = 0;
    stack[0].seg = get_next_segment(&p, &len);
    stack[0].seg_len = len;
    stack[0].p = p;

    while (depth >= 0) {
        router_stack_frame_t* frame = &stack[depth];
        csilk_router_node_t*  curr = frame->node;
        const char*           curr_path = frame->path;

        /* 1. Terminal leaf check: empty or single slash path */
        if (!frame->checked_terminal) {
            frame->checked_terminal = 1;
            if (!curr_path || *curr_path == '\0' || (curr_path[0] == '/' && curr_path[1] == '\0')) {
                csilk_method_handler_t* mh = curr->handlers;
                while (mh) {
                    if (strcmp(mh->method, method) == 0) {
                        if (out_mh) {
                            *out_mh = mh;
                        }
                        CSILK_LOG_T("Router: matched handler for method '%s' at node '%s'",
                                    method,
                                    curr->segment[0] ? curr->segment : "/");
                        return mh->handlers;
                    }
                    mh = mh->next;
                }
            }
        }

        /* 2. Check depth bound */
        if (depth >= CSILK_ROUTER_MAX_DEPTH - 1) {
            if (frame->param_added && ctx) {
                rollback_param(ctx, frame->params_count_snapshot);
                frame->param_added = 0;
            }
            depth--;
            continue;
        }

        /* 3. Try children in priority order */
        csilk_router_node_t** children = node_children(curr);
        int                   pushed_child = 0;

        while (frame->child_idx < curr->children_count) {
            int                  i = frame->child_idx++;
            csilk_router_node_t* child = children[i];

            if (child->type == CSILK_NODE_STATIC) {
                if (!frame->seg) {
                    continue;
                }
                if (child->segment_len == frame->seg_len && child->segment[0] == frame->seg[0]) {
                    int match = 0;
                    if (use_simd) {
                        match = csilk_memcmp_fast(child->segment, frame->seg, frame->seg_len);
                    } else {
                        match = (memcmp(child->segment, frame->seg, frame->seg_len) == 0);
                    }
                    if (match) {
                        int next_depth = depth + 1;
                        stack[next_depth].node = child;
                        stack[next_depth].path = frame->p;
                        stack[next_depth].child_idx = 0;
                        stack[next_depth].params_count_snapshot = ctx ? ctx->params_count : 0;
                        stack[next_depth].param_added = 0;
                        stack[next_depth].checked_terminal = 0;

                        const char* next_p = frame->p;
                        size_t      next_len = 0;
                        stack[next_depth].seg = get_next_segment(&next_p, &next_len);
                        stack[next_depth].seg_len = next_len;
                        stack[next_depth].p = next_p;

                        depth = next_depth;
                        pushed_child = 1;
                        break;
                    }
                }
            } else if (child->type == CSILK_NODE_PARAM) {
                if (!frame->seg) {
                    continue;
                }
                int snapshot = ctx ? ctx->params_count : 0;
                int added = capture_param(ctx, child->segment, frame->seg, frame->seg_len);

                int next_depth = depth + 1;
                stack[next_depth].node = child;
                stack[next_depth].path = frame->p;
                stack[next_depth].child_idx = 0;
                stack[next_depth].params_count_snapshot = snapshot;
                stack[next_depth].param_added = added;
                stack[next_depth].checked_terminal = 0;

                const char* next_p = frame->p;
                size_t      next_len = 0;
                stack[next_depth].seg = get_next_segment(&next_p, &next_len);
                stack[next_depth].seg_len = next_len;
                stack[next_depth].p = next_p;

                depth = next_depth;
                pushed_child = 1;
                break;
            } else if (child->type == CSILK_NODE_WILDCARD) {
                int snapshot = ctx ? ctx->params_count : 0;
                int added = capture_wildcard_param(ctx, child->segment, curr_path);

                csilk_method_handler_t* mh = child->handlers;
                while (mh) {
                    if (strcmp(mh->method, method) == 0) {
                        if (out_mh) {
                            *out_mh = mh;
                        }
                        CSILK_LOG_T("Router: matched handler for method '%s' at wildcard node '%s'",
                                    method,
                                    child->segment);
                        return mh->handlers;
                    }
                    mh = mh->next;
                }

                if (added && ctx) {
                    rollback_param(ctx, snapshot);
                }
            }
        }

        if (pushed_child) {
            continue;
        }

        /* 4. Backtrack from this frame */
        if (frame->param_added && ctx) {
            rollback_param(ctx, frame->params_count_snapshot);
            frame->param_added = 0;
        }
        depth--;
    }

    return NULL;
}
