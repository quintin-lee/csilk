#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static void
test_request_id_null_context()
{
	printf("Testing request_id middleware with NULL context...\n");
	csilk_request_id_middleware(NULL);
	printf("request_id NULL context passed!\n");
}

static void
test_request_id_generates_id()
{
	printf("Testing request_id middleware generates UUID...\n");
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;

	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_request_id_middleware(&ctx);

	assert(ctx.request_id[0] != '\0');
	assert(strlen(ctx.request_id) == 36);

	const char* hdr = csilk_get_response_header(&ctx, "X-Request-Id");
	assert(hdr != NULL);
	assert(strcmp(hdr, ctx.request_id) == 0);

	csilk_arena_free(ctx.arena);
	printf("request_id generates UUID passed!\n");
}

static void
test_request_id_preserves_existing()
{
	printf("Testing request_id middleware preserves existing ID...\n");
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	strcpy(ctx.request_id, "abc-123-def");

	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_request_id_middleware(&ctx);

	assert(strcmp(ctx.request_id, "abc-123-def") == 0);

	const char* hdr = csilk_get_response_header(&ctx, "X-Request-Id");
	assert(hdr != NULL);
	assert(strcmp(hdr, "abc-123-def") == 0);

	csilk_arena_free(ctx.arena);
	printf("request_id preserves existing ID passed!\n");
}

static void
test_health_check_handler_null()
{
	printf("Testing health check handler with NULL...\n");
	csilk_health_check_handler(NULL);
	printf("health check NULL passed!\n");
}

static void
test_health_check_handler()
{
	printf("Testing health check handler...\n");
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));

	csilk_health_check_handler(&ctx);

	assert(ctx.response.status == CSILK_STATUS_OK);
	assert(ctx.response.body != NULL);
	assert(strstr(ctx.response.body, "status") != NULL);
	assert(strstr(ctx.response.body, "up") != NULL);
	assert(ctx.response.body_is_managed == 1);

	if (ctx.response.body && ctx.response.body_is_managed) {
		free((void*)ctx.response.body);
	}
	printf("health check handler passed!\n");
}

int
main()
{
	test_request_id_null_context();
	test_request_id_generates_id();
	test_request_id_preserves_existing();
	test_health_check_handler_null();
	test_health_check_handler();
	printf("test_request_id: ALL PASSED\n");
	return 0;
}
