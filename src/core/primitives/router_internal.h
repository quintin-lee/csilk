/**
 * @file router_internal.h
 * @brief Internal router declarations shared across split translation units.
 *
 * Exposes symbols from router.c, router_match.c, and router_api.c
 * that need cross-file visibility.
 *
 * @copyright MIT License
 */

#ifndef CSILK_ROUTER_INTERNAL_H
#define CSILK_ROUTER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "csilk/core/router.h"

/** @brief Maximum number of children per router tree node. */
enum { CSILK_MAX_CHILDREN = 128 };

/** @brief Node type for router trie. */
typedef enum { CSILK_NODE_STATIC, CSILK_NODE_PARAM, CSILK_NODE_WILDCARD } csilk_node_type_t;

/** @brief Node in the router trie — represents a URL path segment. */
struct csilk_router_node_s {
    char*                       segment;
    size_t                      segment_len;
    csilk_node_type_t           type;
    csilk_method_handler_t*     handlers;
    struct csilk_router_node_s* children[CSILK_MAX_CHILDREN];
    int                         children_count;
};

csilk_router_node_t* node_new(const char* segment, csilk_node_type_t type);
void                 node_free(csilk_router_node_t* node);

const char* get_next_segment(const char** p, size_t* len);
const char* csilk_simd_find_char(const char* s, size_t len, char target);
int         csilk_memcmp_fast(const char* s1, const char* s2, size_t n);

csilk_handler_t* match_node(csilk_router_node_t*     node,
                            const char*              method,
                            const char*              path,
                            csilk_ctx_t*             ctx,
                            csilk_method_handler_t** out_mh);

int router_add_full(csilk_router_t*  r,
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

#endif /* CSILK_ROUTER_INTERNAL_H */
