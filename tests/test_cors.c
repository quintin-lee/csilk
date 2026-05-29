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
test_cors_basic()
{
	printf("Testing basic CORS middleware...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_config_t config = {
	    .allow_origin = "*", .allow_methods = "GET,POST", .allow_headers = "Content-Type"};

	csilk_cors_middleware(ctx, &config);

	assert(strcmp(get_response_header(ctx, "Access-Control-Allow-Origin"), "*") == 0);
	assert(strcmp(get_response_header(ctx, "Access-Control-Allow-Methods"), "GET,POST") == 0);
	assert(strcmp(get_response_header(ctx, "Access-Control-Allow-Headers"), "Content-Type") ==
	       0);

	csilk_test_ctx_free(ctx);
	printf("test_cors_basic passed\n");
}

void
test_cors_options_preflight()
{
	printf("Testing CORS OPTIONS preflight...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "OPTIONS", "/test");
	csilk_set_request_header(ctx, "Access-Control-Request-Method", "POST");

	csilk_cors_config_t config = {.allow_origin = "http://example.com",
				      .allow_methods = "POST"};

	csilk_cors_middleware(ctx, &config);

	assert(csilk_get_status(ctx) == CSILK_STATUS_NO_CONTENT);
	assert(strcmp(get_response_header(ctx, "Access-Control-Allow-Origin"),
		      "http://example.com") == 0);
	assert(csilk_is_aborted(ctx) == 1);

	csilk_test_ctx_free(ctx);
	printf("test_cors_options_preflight passed\n");
}

void
test_cors_null_config()
{
	printf("Testing CORS with NULL config...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_middleware(ctx, NULL);

	assert(get_response_header(ctx, "Access-Control-Allow-Origin") == NULL);

	csilk_test_ctx_free(ctx);
	printf("test_cors_null_config passed\n");
}

void
test_cors_credentials()
{
	printf("Testing CORS with credentials...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_cors_config_t config = {.allow_origin = "http://example.com", .allow_credentials = 1};

	csilk_cors_middleware(ctx, &config);

	assert(strcmp(get_response_header(ctx, "Access-Control-Allow-Credentials"), "true") == 0);

	csilk_test_ctx_free(ctx);
	printf("test_cors_credentials passed\n");
}

int
main()
{
	test_cors_basic();
	test_cors_options_preflight();
	test_cors_null_config();
	test_cors_credentials();
	return 0;
}
