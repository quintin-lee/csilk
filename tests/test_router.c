#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"

void
mock_handler1(csilk_ctx_t* c)
{
    (void)c;
}
void
mock_handler2(csilk_ctx_t* c)
{
    (void)c;
}

void
test_router_simd()
{
    printf("Testing SIMD routing paths...\n");
    csilk_router_t* r = csilk_router_new();
    csilk_handler_t h1[] = {mock_handler1};

    /* Long segment (>32 chars) for AVX2 */
    const char* long_path = "/this_is_a_very_long_path_segment_that_should_trigger_avx2_matching";
    csilk_router_add(r, "GET", long_path, h1, 1);

    csilk_handler_t* matched = csilk_router_match(r, "GET", long_path);
    assert(matched != nullptr && matched[0] == mock_handler1);

    /* Test with prefix match but different tail */
    assert(csilk_router_match(
               r, "GET", "/this_is_a_very_long_path_segment_that_should_trigger_avx2_matchinx") ==
           nullptr);

    /* Extra long segment (>64 chars) for AVX-512 */
    const char* extra_long_path = "/this_is_an_extremely_long_path_segment_specifically_"
                                  "designed_to_trigger_avx512_vectorized_matching";
    csilk_router_add(r, "GET", extra_long_path, h1, 1);

    matched = csilk_router_match(r, "GET", extra_long_path);
    assert(matched != nullptr && matched[0] == mock_handler1);

    assert(csilk_router_match(r,
                              "GET",
                              "/this_is_an_extremely_long_path_segment_specifically_designed_"
                              "to_trigger_avx512_vectorized_matchinx") == nullptr);

    csilk_router_free(r);
    printf("test_router_simd passed!\n");
}

int
main()
{
    test_router_simd();
    csilk_router_t* r = csilk_router_new();
    assert(r != nullptr);

    csilk_handler_t h1[] = {mock_handler1};
    csilk_handler_t h2[] = {mock_handler2};

    // Boundary cases for adding routes
    csilk_router_add(nullptr, "GET", "/hello", h1, 1);
    csilk_router_add(r, nullptr, "/hello", h1, 1);
    csilk_router_add(r, "GET", nullptr, h1, 1);
    csilk_router_add(r, "GET", "/hello", nullptr, 1);

    csilk_router_add(r, "GET", "/hello", h1, 1);
    csilk_router_add(r, "POST", "/submit", h2, 1);

    csilk_handler_t* matched;

    // Boundary cases for matching
    matched = csilk_router_match(nullptr, "GET", "/hello");
    assert(matched == nullptr);
    matched = csilk_router_match(r, nullptr, "/hello");
    assert(matched == nullptr);
    matched = csilk_router_match(r, "GET", nullptr);
    assert(matched == nullptr);

    matched = csilk_router_match(r, "GET", "/hello");
    assert(matched != nullptr && matched[0] == mock_handler1);

    matched = csilk_router_match(r, "POST", "/submit");
    assert(matched != nullptr && matched[0] == mock_handler2);

    assert(csilk_router_match(r, "GET", "/notfound") == nullptr);
    assert(csilk_router_match(r, "POST", "/hello") == nullptr);

    // Match root path corner case
    csilk_router_add(r, "GET", "/", h1, 1);
    matched = csilk_router_match(r, "GET", "/");
    assert(matched != nullptr && matched[0] == mock_handler1);

    csilk_router_free(r);

    // Test double free safety
    csilk_router_free(nullptr);

    printf("test_router passed!\n");
    return 0;
}
