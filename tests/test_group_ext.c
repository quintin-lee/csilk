#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static int test_handler_called = 0;

static void
test_handler(csilk_ctx_t* c)
{
	test_handler_called++;
	csilk_string(c, CSILK_STATUS_OK, "ok");
}

int
main()
{
	csilk_router_t* r = csilk_router_new();

	printf("Testing csilk_group_add_route_extended with NULL params...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/nulltest");
		csilk_group_add_route_extended(
		    NULL, "GET", "/test", test_handler, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended(
		    g, NULL, "/test", test_handler, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended(
		    g, "GET", NULL, test_handler, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended(g, "GET", "/test", NULL, NULL, NULL, NULL, NULL);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended with basic metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/api");
		assert(g != NULL);
		csilk_group_add_route_extended(g,
					       "GET",
					       "/hello",
					       test_handler,
					       "InputType",
					       "OutputType",
					       "Hello summary",
					       "Hello description");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "GET";
		ctx.request.path = strdup("/api/hello");
		int matched = csilk_router_match_ctx(r, &ctx);
		assert(matched);
		test_handler_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(test_handler_called == 1);
		assert(ctx.response.status == CSILK_STATUS_OK);
		csilk_ctx_cleanup(&ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended with NULL metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/v2");
		assert(g != NULL);
		csilk_group_add_route_extended(
		    g, "POST", "/data", test_handler, NULL, NULL, NULL, NULL);
		csilk_ctx_t ctx = {0};
		ctx.request.method = "POST";
		ctx.request.path = strdup("/v2/data");
		int matched = csilk_router_match_ctx(r, &ctx);
		assert(matched);
		test_handler_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(test_handler_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/admin");
		assert(g != NULL);
		csilk_group_add_route_extended_perm(g,
						    "DELETE",
						    "/user/:id",
						    test_handler,
						    NULL,
						    NULL,
						    "Delete user",
						    "Delete a user by ID",
						    "admin",
						    "users");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "DELETE";
		ctx.request.path = strdup("/admin/user/42");
		int matched = csilk_router_match_ctx(r, &ctx);
		assert(matched);
		test_handler_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(test_handler_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm with NULL "
	       "params...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/nullperm");
		csilk_group_add_route_extended_perm(
		    NULL, "GET", "/test", test_handler, NULL, NULL, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended_perm(
		    g, NULL, "/test", test_handler, NULL, NULL, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended_perm(
		    g, "GET", NULL, test_handler, NULL, NULL, NULL, NULL, NULL, NULL);
		csilk_group_add_route_extended_perm(
		    g, "GET", "/test", NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm with full "
	       "metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/api/v3");
		assert(g != NULL);
		csilk_group_add_route_extended_perm(g,
						    "PUT",
						    "/item/:id",
						    test_handler,
						    "ItemInput",
						    "ItemOutput",
						    "Update item",
						    "Update an item by ID",
						    "write",
						    "items:*");
		csilk_ctx_t ctx = {0};
		ctx.request.method = "PUT";
		ctx.request.path = strdup("/api/v3/item/99");
		int matched = csilk_router_match_ctx(r, &ctx);
		assert(matched);
		test_handler_called = 0;
		ctx.handler_index = -1;
		csilk_next(&ctx);
		assert(test_handler_called == 1);
		csilk_ctx_cleanup(&ctx);
		csilk_group_free(g);
	}

	csilk_router_free(r);

	printf("test_group_ext: PASS\n");
	return 0;
}
