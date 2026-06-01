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
	const char* long_path =
	    "/this_is_a_very_long_path_segment_that_should_trigger_avx2_matching";
	csilk_router_add(r, "GET", long_path, h1, 1);

	csilk_handler_t* matched = csilk_router_match(r, "GET", long_path);
	assert(matched != NULL && matched[0] == mock_handler1);

	/* Test with prefix match but different tail */
	assert(csilk_router_match(
		   r,
		   "GET",
		   "/this_is_a_very_long_path_segment_that_should_trigger_avx2_matchinx") == NULL);

	csilk_router_free(r);
	printf("test_router_simd passed!\n");
}

int
main()
{
	test_router_simd();
	csilk_router_t* r = csilk_router_new();
	assert(r != NULL);

	csilk_handler_t h1[] = {mock_handler1};
	csilk_handler_t h2[] = {mock_handler2};

	// Boundary cases for adding routes
	csilk_router_add(NULL, "GET", "/hello", h1, 1);
	csilk_router_add(r, NULL, "/hello", h1, 1);
	csilk_router_add(r, "GET", NULL, h1, 1);
	csilk_router_add(r, "GET", "/hello", NULL, 1);

	csilk_router_add(r, "GET", "/hello", h1, 1);
	csilk_router_add(r, "POST", "/submit", h2, 1);

	csilk_handler_t* matched;

	// Boundary cases for matching
	matched = csilk_router_match(NULL, "GET", "/hello");
	assert(matched == NULL);
	matched = csilk_router_match(r, NULL, "/hello");
	assert(matched == NULL);
	matched = csilk_router_match(r, "GET", NULL);
	assert(matched == NULL);

	matched = csilk_router_match(r, "GET", "/hello");
	assert(matched != NULL && matched[0] == mock_handler1);

	matched = csilk_router_match(r, "POST", "/submit");
	assert(matched != NULL && matched[0] == mock_handler2);

	assert(csilk_router_match(r, "GET", "/notfound") == NULL);
	assert(csilk_router_match(r, "POST", "/hello") == NULL);

	// Match root path corner case
	csilk_router_add(r, "GET", "/", h1, 1);
	matched = csilk_router_match(r, "GET", "/");
	assert(matched != NULL && matched[0] == mock_handler1);

	csilk_router_free(r);

	// Test double free safety
	csilk_router_free(NULL);

	printf("test_router passed!\n");
	return 0;
}
