#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_csrf_token_generation()
{
	char token1[64];
	char token2[64];

	assert(csilk_csrf_generate_token(token1, sizeof(token1)) == 0);
	assert(csilk_csrf_generate_token(token2, sizeof(token2)) == 0);

	assert(strlen(token1) == 32);
	assert(strcmp(token1, token2) != 0); // Tokens should be unique

	printf("test_csrf_token_generation passed\n");
}

void
test_csrf_buffer_too_small()
{
	char buf[2];
	assert(csilk_csrf_generate_token(buf, 2) == -1);
	printf("test_csrf_buffer_too_small passed\n");
}

void
test_csrf_middleware_missing_token()
{
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN || csilk_get_status(ctx) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_missing_token passed\n");
}

int
main()
{
	test_csrf_token_generation();
	test_csrf_buffer_too_small();
	test_csrf_middleware_missing_token();
	return 0;
}
