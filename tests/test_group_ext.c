#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

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

	printf("Testing csilk_group_add_route_extended with nullptr params...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/nulltest");
		csilk_group_add_route_extended(
		    nullptr, "GET", "/test", test_handler, nullptr, nullptr, nullptr, nullptr);
		csilk_group_add_route_extended(
		    g, nullptr, "/test", test_handler, nullptr, nullptr, nullptr, nullptr);
		csilk_group_add_route_extended(
		    g, "GET", nullptr, test_handler, nullptr, nullptr, nullptr, nullptr);
		csilk_group_add_route_extended(
		    g, "GET", "/test", nullptr, nullptr, nullptr, nullptr, nullptr);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended with basic metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/api");
		assert(g != nullptr);
		csilk_group_add_route_extended(g,
					       "GET",
					       "/hello",
					       test_handler,
					       "InputType",
					       "OutputType",
					       "Hello summary",
					       "Hello description");

		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "GET", "/api/hello");

		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched);
		test_handler_called = 0;
		csilk_next(ctx);
		assert(test_handler_called == 1);
		assert(csilk_get_status(ctx) == CSILK_STATUS_OK);

		csilk_test_ctx_free(ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended with nullptr metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/v2");
		assert(g != nullptr);
		csilk_group_add_route_extended(
		    g, "POST", "/data", test_handler, nullptr, nullptr, nullptr, nullptr);

		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "POST", "/v2/data");

		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched);
		test_handler_called = 0;
		csilk_next(ctx);
		assert(test_handler_called == 1);

		csilk_test_ctx_free(ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/admin");
		assert(g != nullptr);
		csilk_group_add_route_extended_perm(g,
						    "DELETE",
						    "/user/:id",
						    test_handler,
						    nullptr,
						    nullptr,
						    "Delete user",
						    "Delete a user by ID",
						    "admin",
						    "users");

		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "DELETE", "/admin/user/42");

		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched);
		test_handler_called = 0;
		csilk_next(ctx);
		assert(test_handler_called == 1);

		csilk_test_ctx_free(ctx);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm with nullptr "
	       "params...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/nullperm");
		csilk_group_add_route_extended_perm(nullptr,
						    "GET",
						    "/test",
						    test_handler,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr);
		csilk_group_add_route_extended_perm(g,
						    nullptr,
						    "/test",
						    test_handler,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr);
		csilk_group_add_route_extended_perm(g,
						    "GET",
						    nullptr,
						    test_handler,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr);
		csilk_group_add_route_extended_perm(g,
						    "GET",
						    "/test",
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr,
						    nullptr);
		csilk_group_free(g);
	}

	printf("Testing csilk_group_add_route_extended_perm with full "
	       "metadata...\n");
	{
		csilk_group_t* g = csilk_group_new(r, "/api/v3");
		assert(g != nullptr);
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

		csilk_ctx_t* ctx = csilk_test_ctx_new();
		csilk_test_ctx_set_request(ctx, "PUT", "/api/v3/item/99");

		int matched = csilk_router_match_ctx(r, ctx);
		assert(matched);
		test_handler_called = 0;
		csilk_next(ctx);
		assert(test_handler_called == 1);

		csilk_test_ctx_free(ctx);
		csilk_group_free(g);
	}

	csilk_router_free(r);

	printf("test_group_ext: PASS\n");
	return 0;
}
