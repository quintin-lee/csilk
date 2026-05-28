#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static void
test_csrf_generate_token_null()
{
	printf("Testing csrf_generate_token with NULL...\n");
	assert(csilk_csrf_generate_token(NULL, 33) == -1);
	printf("csrf_generate_token NULL passed!\n");
}

static void
test_csrf_generate_token_small_buffer()
{
	printf("Testing csrf_generate_token with small buffer...\n");
	char buf[10];
	assert(csilk_csrf_generate_token(buf, 10) == -1);
	printf("csrf_generate_token small buffer passed!\n");
}

static void
test_csrf_generate_token_valid()
{
	printf("Testing csrf_generate_token valid...\n");
	char buf[64];
	assert(csilk_csrf_generate_token(buf, sizeof(buf)) == 0);
	assert(strlen(buf) == 32);
	printf("csrf_generate_token valid passed! token=%s\n", buf);
}

static void
test_csrf_generate_token_unique()
{
	printf("Testing csrf_generate_token unique...\n");
	char buf1[64], buf2[64];
	csilk_csrf_generate_token(buf1, sizeof(buf1));
	csilk_csrf_generate_token(buf2, sizeof(buf2));
	assert(strcmp(buf1, buf2) != 0);
	printf("csrf_generate_token unique passed!\n");
}

static void
test_csrf_middleware_safe_method_get()
{
	printf("Testing csrf middleware on GET (safe method)...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "GET";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_csrf_middleware(&ctx);
	assert(ctx.response.status == 0);
	assert(ctx.aborted == 0);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware GET passed!\n");
}

static void
test_csrf_middleware_safe_method_head()
{
	printf("Testing csrf middleware on HEAD (safe method)...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "HEAD";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_csrf_middleware(&ctx);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware HEAD passed!\n");
}

static void
test_csrf_middleware_safe_method_options()
{
	printf("Testing csrf middleware on OPTIONS (safe method)...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "OPTIONS";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_csrf_middleware(&ctx);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware OPTIONS passed!\n");
}

static void
test_csrf_middleware_post_no_token()
{
	printf("Testing csrf middleware on POST without CSRF token...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "POST";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_csrf_middleware(&ctx);
	assert(ctx.aborted == 1);
	assert(ctx.response.status == CSILK_STATUS_FORBIDDEN);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware POST no token passed!\n");
}

static void
test_csrf_middleware_post_with_matching_token()
{
	printf("Testing csrf middleware on POST with matching token...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "POST";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_set_request_header(&ctx, "Cookie", "csrf_token=abc123");
	csilk_set_request_header(&ctx, "X-CSRF-Token", "abc123");

	csilk_csrf_middleware(&ctx);
	assert(ctx.aborted == 0);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware POST matching token passed!\n");
}

static void
test_csrf_middleware_post_with_wrong_token()
{
	printf("Testing csrf middleware on POST with wrong token...\n");
	csilk_ctx_t ctx = {0};
	ctx.arena = csilk_arena_new(1024);
	ctx.handler_index = -1;
	ctx.request.method = "POST";
	csilk_handler_t handlers[] = {NULL};
	ctx.handlers = handlers;

	csilk_set_request_header(&ctx, "Cookie", "csrf_token=abc123");
	csilk_set_request_header(&ctx, "X-CSRF-Token", "wrongtoken");

	csilk_csrf_middleware(&ctx);
	assert(ctx.aborted == 1);
	assert(ctx.response.status == CSILK_STATUS_FORBIDDEN);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("csrf middleware POST wrong token passed!\n");
}

int
main()
{
	test_csrf_generate_token_null();
	test_csrf_generate_token_small_buffer();
	test_csrf_generate_token_valid();
	test_csrf_generate_token_unique();
	test_csrf_middleware_safe_method_get();
	test_csrf_middleware_safe_method_head();
	test_csrf_middleware_safe_method_options();
	test_csrf_middleware_post_no_token();
	test_csrf_middleware_post_with_matching_token();
	test_csrf_middleware_post_with_wrong_token();
	printf("test_csrf_ext: ALL PASSED\n");
	return 0;
}
