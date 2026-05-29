#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
dummy_handler(csilk_ctx_t* c)
{
	(void)c;
}

int
main()
{
	printf("Testing URL parameters limit (CSILK_MAX_PARAMS = 20)...\n");

	csilk_router_t* router = csilk_router_new();

	// Create a route with 25 parameters
	char route[1024] = "";
	for (int i = 1; i <= 25; i++) {
		char seg[32];
		snprintf(seg, sizeof(seg), "/:p%d", i);
		strcat(route, seg);
	}

	csilk_handler_t handlers[] = {dummy_handler};
	csilk_router_add(router, "GET", route, handlers, 1);

	// Create a request path with 25 segments
	char path[1024] = "";
	for (int i = 1; i <= 25; i++) {
		char seg[32];
		snprintf(seg, sizeof(seg), "/v%d", i);
		strcat(path, seg);
	}

	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "GET", path);

	int matched = csilk_router_match_ctx(router, ctx);

	assert(matched == 1);
	assert(csilk_get_params_count(ctx) == CSILK_MAX_PARAMS);

	// Verify first 20 params
	for (int i = 0; i < CSILK_MAX_PARAMS; i++) {
		char expected_key[32];
		char expected_val[32];
		snprintf(expected_key, sizeof(expected_key), "p%d", i + 1);
		snprintf(expected_val, sizeof(expected_val), "v%d", i + 1);

		assert(strcmp(csilk_get_param_key(ctx, i), expected_key) == 0);
		assert(strcmp(csilk_get_param_value(ctx, i), expected_val) == 0);
	}

	csilk_test_ctx_free(ctx);
	csilk_router_free(router);
	printf("test_params_limit: PASS\n");
	return 0;
}
