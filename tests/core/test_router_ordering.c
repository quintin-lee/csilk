/**
 * @file test_router_ordering.c
 * @brief Unit tests, ordering invariant assertions, and fuzz testing for router insertion.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/router.h"
#include "core/primitives/router_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
h_static(csilk_ctx_t* c)
{
    (void)c;
}
static void
h_param(csilk_ctx_t* c)
{
    (void)c;
}
static void
h_wildcard(csilk_ctx_t* c)
{
    (void)c;
}

static void
assert_tree_invariants(csilk_router_node_t* node)
{
    if (!node) {
        return;
    }

    csilk_router_node_t** children = node_children(node);
    for (int i = 0; i < node->children_count - 1; i++) {
        csilk_router_node_t* a = children[i];
        csilk_router_node_t* b = children[i + 1];

        /* Type ordering: STATIC (0) <= PARAM (1) <= WILDCARD (2) */
        assert(a->type <= b->type);

        /* Within same type: strictly sorted by segment */
        if (a->type == b->type) {
            int cmp = strcmp(a->segment, b->segment);
            assert(cmp < 0);
        }
    }

    for (int i = 0; i < node->children_count; i++) {
        assert_tree_invariants(children[i]);
    }
}

static void
test_strict_type_priority_ordering(void)
{
    printf("Testing strict STATIC < PARAM < WILDCARD priority ordering...\n");

    csilk_router_t* r = csilk_router_new();
    assert(r != NULL);

    csilk_handler_t hs[] = {h_static, NULL};
    csilk_handler_t hp[] = {h_param, NULL};
    csilk_handler_t hw[] = {h_wildcard, NULL};

    /* 1. Insert in reverse priority: WILDCARD -> PARAM -> STATIC */
    csilk_router_add(r, "GET", "/api/*rest", hw, 1);
    csilk_router_add(r, "GET", "/api/:id", hp, 1);
    csilk_router_add(r, "GET", "/api/login", hs, 1);
    csilk_router_add(r, "GET", "/api/about", hs, 1);
    csilk_router_add(r, "GET", "/api/users", hs, 1);

    /* Verify tree ordering invariant */
    assert_tree_invariants(r->root);

    /* Match exact static */
    csilk_handler_t* m_login = csilk_router_match(r, "GET", "/api/login");
    assert(m_login != NULL && m_login[0] == h_static);

    csilk_handler_t* m_about = csilk_router_match(r, "GET", "/api/about");
    assert(m_about != NULL && m_about[0] == h_static);

    /* Match param */
    csilk_handler_t* m_param = csilk_router_match(r, "GET", "/api/12345");
    assert(m_param != NULL && m_param[0] == h_param);

    /* Match wildcard */
    csilk_handler_t* m_wild = csilk_router_match(r, "GET", "/api/multi/level/path");
    assert(m_wild != NULL && m_wild[0] == h_wildcard);

    csilk_router_free(r);
    printf("  Strict priority ordering test passed!\n");
}

static void
test_deep_hierarchical_shadowing(void)
{
    printf("Testing deep hierarchical shadowing and exact resolution...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_handler_t hs[] = {h_static, NULL};
    csilk_handler_t hp[] = {h_param, NULL};
    csilk_handler_t hw[] = {h_wildcard, NULL};

    /* Register in chaotic order */
    csilk_router_add(r, "GET", "/v1/*catchall", hw, 1);
    csilk_router_add(r, "GET", "/v1/users/:uid/posts/:pid/comments", hp, 1);
    csilk_router_add(r, "GET", "/v1/users/:uid/posts/pinned", hs, 1);
    csilk_router_add(r, "GET", "/v1/users/me/profile", hs, 1);
    csilk_router_add(r, "GET", "/v1/users/me/posts", hs, 1);
    csilk_router_add(r, "GET", "/v1/users/:uid", hp, 1);

    assert_tree_invariants(r->root);

    /* Static priority on /v1/users/me/profile */
    csilk_handler_t* m1 = csilk_router_match(r, "GET", "/v1/users/me/profile");
    assert(m1 != NULL && m1[0] == h_static);

    /* Static pinned under param uid */
    csilk_handler_t* m2 = csilk_router_match(r, "GET", "/v1/users/user42/posts/pinned");
    assert(m2 != NULL && m2[0] == h_static);

    /* Param comments under param uid and param pid */
    csilk_handler_t* m3 = csilk_router_match(r, "GET", "/v1/users/user42/posts/p999/comments");
    assert(m3 != NULL && m3[0] == h_param);

    /* Wildcard catchall */
    csilk_handler_t* m4 = csilk_router_match(r, "GET", "/v1/other/service/call");
    assert(m4 != NULL && m4[0] == h_wildcard);

    csilk_router_free(r);
    printf("  Deep hierarchical shadowing test passed!\n");
}

static void
test_fuzz_route_registration(void)
{
    printf("Fuzzing route registration (5,000 randomized routes)...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_handler_t hs[] = {h_static, NULL};

    char     route_buf[128];
    uint32_t seed = 0x12345678;

    for (int i = 0; i < 5000; i++) {
        /* Pseudo-random route generation */
        seed = seed * 1664525ULL + 1013904223ULL;
        int depth = 1 + (seed % 4);
        int pos = 0;
        pos += snprintf(route_buf + pos, sizeof(route_buf) - (size_t)pos, "/");

        for (int d = 0; d < depth; d++) {
            seed = seed * 1664525ULL + 1013904223ULL;
            int type = seed % 10;
            if (type < 7) {
                /* Static segment */
                int seg_id = seed % 30;
                pos += snprintf(route_buf + pos, sizeof(route_buf) - (size_t)pos, "seg%d/", seg_id);
            } else if (type < 9) {
                /* Param segment */
                int param_id = seed % 5;
                pos +=
                    snprintf(route_buf + pos, sizeof(route_buf) - (size_t)pos, ":p%d/", param_id);
            } else {
                /* Wildcard segment (terminal) */
                pos += snprintf(route_buf + pos, sizeof(route_buf) - (size_t)pos, "*wild");
                break;
            }
        }
        /* Strip trailing slash */
        if (pos > 1 && route_buf[pos - 1] == '/') {
            route_buf[pos - 1] = '\0';
        }

        csilk_router_add(r, "GET", route_buf, hs, 1);

        if (i % 1000 == 0) {
            assert_tree_invariants(r->root);
        }
    }

    assert_tree_invariants(r->root);
    csilk_router_free(r);
    printf("  Fuzz testing passed with 100%% invariant validation!\n");
}

int
main(void)
{
    printf("=== Csilk Router Ordering & Fuzz Invariant Test Suite ===\n\n");
    test_strict_type_priority_ordering();
    test_deep_hierarchical_shadowing();
    test_fuzz_route_registration();
    printf("\n=== All route ordering and fuzz tests passed successfully! ===\n");
    return EXIT_SUCCESS;
}
