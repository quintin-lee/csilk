#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
mock_handler(csilk_ctx_t* c)
{
	(void)c;
}

int
main()
{
	csilk_router_t* r = csilk_router_new();

	csilk_handler_t h1[] = {mock_handler};
	csilk_router_add(r, "GET", "/api/v1/users", h1, 1);
	csilk_router_add(r, "GET", "/api/v1/users/:id", h1, 1);
	csilk_router_add(r, "GET", "/api/v1/posts/*path", h1, 1);
	csilk_router_add(r, "POST", "/api/v1/users", h1, 1);

	// Test static match
	{
		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "GET", "/api/v1/users");
		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched == 1);
		csilk_test_ctx_free(ctx);
	}

	// Test param match
	{
		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "GET", "/api/v1/users/123");
		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched == 1);
		assert(strcmp(csilk_get_param(ctx, "id"), "123") == 0);
		csilk_test_ctx_free(ctx);
	}

	// Test wildcard match
	{
		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "GET", "/api/v1/posts/2023/05/hello");
		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched == 1);
		assert(strcmp(csilk_get_param(ctx, "path"), "2023/05/hello") == 0);
		csilk_test_ctx_free(ctx);
	}

	// Test method mismatch
	{
		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "PUT", "/api/v1/users");
		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched == 0);
		csilk_test_ctx_free(ctx);
	}

	// Test no match
	{
		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "GET", "/api/v1/undefined");
		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched == 0);
		csilk_test_ctx_free(ctx);
	}

	csilk_router_free(r);
	printf("test_radix: PASS\n");
	return 0;
}
