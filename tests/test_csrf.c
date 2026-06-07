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
test_csrf_null_buffer()
{
	assert(csilk_csrf_generate_token(nullptr, 33) == -1);
	printf("test_csrf_null_buffer passed\n");
}

void
test_csrf_buffer_exact_size()
{
	char buf[33];
	assert(csilk_csrf_generate_token(buf, sizeof(buf)) == 0);
	assert(strlen(buf) == 32);
	printf("test_csrf_buffer_exact_size passed\n");
}

void
test_csrf_safe_methods()
{
	/* GET should pass through (set cookie, call next) */
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "GET", "/test");

	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	/* HEAD should also pass through */
	ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "HEAD", "/test");
	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	/* OPTIONS should also pass through */
	ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "OPTIONS", "/test");
	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	printf("test_csrf_safe_methods passed\n");
}

void
test_csrf_unsafe_methods_missing_token()
{
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_test_ctx_set_request(ctx, "POST", "/submit");
	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "PUT", "/update");
	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "DELETE", "/remove");
	csilk_csrf_middleware(ctx);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);
	csilk_test_ctx_free(ctx);

	printf("test_csrf_unsafe_methods_missing_token passed\n");
}

void
test_csrf_token_hex_format()
{
	char token[64];
	assert(csilk_csrf_generate_token(token, sizeof(token)) == 0);
	/* All 32 characters must be hex (0-9, a-f) */
	for (int i = 0; i < 32; i++) {
		char c = token[i];
		assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
	}
	printf("test_csrf_token_hex_format passed\n");
}

int
main()
{
	test_csrf_token_generation();
	test_csrf_buffer_too_small();
	test_csrf_null_buffer();
	test_csrf_buffer_exact_size();
	test_csrf_safe_methods();
	test_csrf_unsafe_methods_missing_token();
	test_csrf_token_hex_format();
	printf("test_csrf: ALL PASSED\n");
	return 0;
}
