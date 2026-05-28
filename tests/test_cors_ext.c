#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static const char*
get_response_header(csilk_ctx_t* ctx, const char* key)
{
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = ctx->response.headers.buckets[i];
		while (h) {
			if (strcasecmp(h->key, key) == 0) {
				return h->value;
			}
			h = h->next;
		}
	}
	return NULL;
}

static void
test_cors_null_context()
{
	printf("Testing CORS with NULL context...\n");
	csilk_cors_middleware(NULL, NULL);
	printf("CORS null context passed!\n");
}

static void
test_cors_null_config_with_context()
{
	printf("Testing CORS with NULL config but valid context...\n");
	csilk_ctx_t ctx = {0};
	ctx.handler_index = -1;
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;
	csilk_cors_middleware(&ctx, NULL);
	printf("CORS null config with context passed!\n");
}

static void
test_cors_allow_credentials()
{
	printf("Testing CORS with allow_credentials...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;

	csilk_cors_config_t config = {.allow_origin = "*",
				      .allow_methods = "GET",
				      .allow_headers = "*",
				      .allow_credentials = 1,
				      .max_age = 86400};

	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_cors_middleware(&ctx, &config);

	const char* creds = get_response_header(&ctx, "Access-Control-Allow-Credentials");
	assert(creds != NULL && strcmp(creds, "true") == 0);

	const char* max_age = get_response_header(&ctx, "Access-Control-Max-Age");
	assert(max_age != NULL && strcmp(max_age, "86400") == 0);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("CORS allow_credentials passed!\n");
}

static void
test_cors_non_wildcard_origin()
{
	printf("Testing CORS with specific origin...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;

	csilk_cors_config_t config = {.allow_origin = "https://example.com",
				      .allow_methods = "POST",
				      .allow_headers = "X-Custom",
				      .allow_credentials = 0,
				      .max_age = 0};

	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_cors_middleware(&ctx, &config);

	const char* origin = get_response_header(&ctx, "Access-Control-Allow-Origin");
	assert(origin != NULL && strcmp(origin, "https://example.com") == 0);

	const char* vary = get_response_header(&ctx, "Vary");
	assert(vary != NULL && strcmp(vary, "Origin") == 0);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("CORS non-wildcard origin passed!\n");
}

static void
test_cors_zero_max_age()
{
	printf("Testing CORS with zero max_age...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;

	csilk_cors_config_t config = {.allow_origin = "*",
				      .allow_methods = "GET",
				      .allow_headers = "*",
				      .allow_credentials = 0,
				      .max_age = 0};

	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_cors_middleware(&ctx, &config);

	const char* max_age = get_response_header(&ctx, "Access-Control-Max-Age");
	assert(max_age == NULL);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("CORS zero max_age passed!\n");
}

int
main()
{
	test_cors_null_context();
	test_cors_null_config_with_context();
	test_cors_allow_credentials();
	test_cors_non_wildcard_origin();
	test_cors_zero_max_age();
	printf("test_cors_ext: ALL PASSED\n");
	return 0;
}
