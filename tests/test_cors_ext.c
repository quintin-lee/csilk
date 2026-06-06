#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static const char*
get_response_header(csilk_ctx_t* ctx, const char* key)
{
	return csilk_get_response_header(ctx, key);
}

void
test_cors_null_config_with_context()
{
	printf("Testing CORS with nullptr config and context...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_middleware(ctx, nullptr);

	assert(get_response_header(ctx, "Access-Control-Allow-Origin") == nullptr);

	csilk_test_ctx_free(ctx);
	printf("test_cors_null_config_with_context passed\n");
}

void
test_cors_allow_credentials()
{
	printf("Testing CORS with allow credentials...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_config_t config = {
	    .allow_origin = "http://localhost", .allow_methods = "GET", .allow_credentials = 1};

	csilk_cors_middleware(ctx, &config);

	const char* allow_creds = get_response_header(ctx, "Access-Control-Allow-Credentials");
	assert(allow_creds != nullptr && strcmp(allow_creds, "true") == 0);

	csilk_test_ctx_free(ctx);
	printf("test_cors_allow_credentials passed\n");
}

void
test_cors_non_wildcard_origin()
{
	printf("Testing CORS with explicit origin...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_config_t config = {.allow_origin = "http://example.com", .allow_methods = "GET"};

	csilk_cors_middleware(ctx, &config);

	const char* allow_origin = get_response_header(ctx, "Access-Control-Allow-Origin");
	assert(allow_origin != nullptr && strcmp(allow_origin, "http://example.com") == 0);

	csilk_test_ctx_free(ctx);
	printf("test_cors_non_wildcard_origin passed\n");
}

void
test_cors_zero_max_age()
{
	printf("Testing CORS with max_age = 0...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_config_t config = {.allow_origin = "*", .allow_methods = "GET", .max_age = 0};

	csilk_cors_middleware(ctx, &config);

	const char* max_age = get_response_header(ctx, "Access-Control-Max-Age");
	// If max_age is 0, the middleware currently doesn't set the header
	assert(max_age == nullptr);

	csilk_test_ctx_free(ctx);
	printf("test_cors_zero_max_age passed\n");
}

int
main()
{
	test_cors_null_config_with_context();
	test_cors_allow_credentials();
	test_cors_non_wildcard_origin();
	test_cors_zero_max_age();
	printf("test_cors_ext: ALL PASSED\n");
	return 0;
}
