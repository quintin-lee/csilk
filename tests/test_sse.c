#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_sse_init_null()
{
	printf("Testing SSE init with nullptr context...\n");
	csilk_sse_init(nullptr);
	printf("SSE init null test passed!\n");
}

static void
test_sse_send_null()
{
	printf("Testing SSE send with nullptr context...\n");
	csilk_sse_send(nullptr, "event", "data");
	csilk_sse_send(nullptr, nullptr, "data");
	csilk_sse_send(nullptr, "event", nullptr);
	printf("SSE send null test passed!\n");
}

static void
test_sse_close_null()
{
	printf("Testing SSE close with nullptr context...\n");
	csilk_sse_close(nullptr);
	printf("SSE close null test passed!\n");
}

static void
test_sse_init_headers()
{
	printf("Testing SSE init sets status and headers...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_sse_init(ctx);
	/* csilk_sse_init needs _internal_client to send headers;
   * when _internal_client is nullptr it still sets up the context */
	assert(csilk_is_sse(ctx) == 1);
	assert(csilk_get_status(ctx) == CSILK_STATUS_OK);

	csilk_test_ctx_free(ctx);
	printf("SSE init headers test passed!\n");
}

int
main()
{
	test_sse_init_null();
	test_sse_send_null();
	test_sse_close_null();
	test_sse_init_headers();
	printf("test_sse: ALL PASSED\n");
	return 0;
}
