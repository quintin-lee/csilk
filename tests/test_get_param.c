#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int
main()
{
	printf("Testing csilk_get_param...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	// No params
	assert(csilk_get_param(ctx, "any") == NULL);

	// With some params
	csilk_test_ctx_add_param(ctx, "id", "123");

	assert(csilk_get_param(ctx, "id") != NULL);
	assert(strcmp(csilk_get_param(ctx, "id"), "123") == 0);
	assert(csilk_get_param(ctx, "other") == NULL);

	// Cleanup
	csilk_test_ctx_free(ctx);

	printf("test_get_param: PASS\n");
	return 0;
}
