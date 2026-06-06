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

void
test_csrf_middleware_safe_method_get()
{
	printf("Testing CSRF middleware safe method GET...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "GET", "/test");

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 1);
	assert(csilk_is_aborted(ctx) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_safe_method_get passed\n");
}

void
test_csrf_middleware_safe_method_head()
{
	printf("Testing CSRF middleware safe method HEAD...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "HEAD", "/test");

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 1);
	assert(csilk_is_aborted(ctx) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_safe_method_head passed\n");
}

void
test_csrf_middleware_safe_method_options()
{
	printf("Testing CSRF middleware safe method OPTIONS...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "OPTIONS", "/test");

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 1);
	assert(csilk_is_aborted(ctx) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_safe_method_options passed\n");
}

void
test_csrf_middleware_post_no_token()
{
	printf("Testing CSRF middleware POST with missing token...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "POST", "/test");

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 0);
	assert(csilk_is_aborted(ctx) == 1);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_post_no_token passed\n");
}

void
test_csrf_middleware_post_with_matching_token()
{
	printf("Testing CSRF middleware POST with matching token...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "POST", "/test");

	char token[64];
	csilk_csrf_generate_token(token, sizeof(token));

	csilk_set_request_header(ctx, "X-CSRF-Token", token);
	// Double-submit pattern needs the same token in a cookie
	csilk_set_request_header(
	    ctx, "Cookie", "csrf_token=DUMMY"); // Just to ensure cookie map exists
	// Wait, csilk_get_cookie reads from request headers.
	// I need to set the cookie in the request headers.
	char cookie_hdr[128];
	snprintf(cookie_hdr, sizeof(cookie_hdr), "csrf_token=%s", token);
	csilk_set_request_header(ctx, "Cookie", cookie_hdr);

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 1);
	assert(csilk_is_aborted(ctx) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_post_with_matching_token passed\n");
}

void
test_csrf_middleware_post_with_wrong_token()
{
	printf("Testing CSRF middleware POST with wrong token...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	csilk_test_ctx_set_request(ctx, "POST", "/test");

	csilk_set_request_header(ctx, "X-CSRF-Token", "wrong-token");
	csilk_set_request_header(ctx, "Cookie", "csrf_token=correct-token");

	csilk_handler_t handlers[] = {test_handler, nullptr};
	csilk_test_ctx_set_handlers(ctx, handlers);

	test_handler_called = 0;
	csilk_csrf_middleware(ctx);

	assert(test_handler_called == 0);
	assert(csilk_is_aborted(ctx) == 1);
	assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);

	csilk_test_ctx_free(ctx);
	printf("test_csrf_middleware_post_with_wrong_token passed\n");
}

int
main()
{
	test_csrf_middleware_safe_method_get();
	test_csrf_middleware_safe_method_head();
	test_csrf_middleware_safe_method_options();
	test_csrf_middleware_post_no_token();
	test_csrf_middleware_post_with_matching_token();
	test_csrf_middleware_post_with_wrong_token();
	printf("test_csrf_ext: ALL PASSED\n");
	return 0;
}
