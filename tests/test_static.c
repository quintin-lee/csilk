#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

// Mock _csilk_send_response
void
_csilk_send_response(csilk_ctx_t* c)
{
	(void)c;
}

static void
setup_test_file()
{
	mkdir("./test_public", 0777);
	FILE* f = fopen("./test_public/hello.txt", "w");
	fputs("Hello, Csilk!", f);
	fclose(f);
}

void
test_static_serves_file()
{
	printf("Testing static file serve...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_set(ctx, "static_prefix", "/static");
	csilk_test_ctx_set_request(ctx, "GET", "/static/hello.txt");

	csilk_static(ctx, "./test_public");

	assert(csilk_is_async(ctx) == 1);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(csilk_get_status(ctx) == CSILK_STATUS_OK);
	assert(csilk_get_file_fd(ctx) != -1);

	csilk_test_ctx_free(ctx);
	printf("test_static_serves_file passed\n");
}

void
test_static_traversal_blocked()
{
	printf("Testing static file traversal protection...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_set(ctx, "static_prefix", "/static");
	csilk_test_ctx_set_request(ctx, "GET", "/static/../secrets.txt");

	csilk_static(ctx, "./test_public");

	assert(csilk_is_async(ctx) == 1);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	// Should be 404 or aborted
	assert(csilk_get_status(ctx) == CSILK_STATUS_NOT_FOUND);

	csilk_test_ctx_free(ctx);
	printf("test_static_traversal_blocked passed\n");
}

void
test_static_range_first_5()
{
	printf("Testing static file Range request (0-4)...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_set(ctx, "static_prefix", "/static");
	csilk_test_ctx_set_request(ctx, "GET", "/static/hello.txt");
	csilk_set_request_header(ctx, "Range", "bytes=0-4");

	csilk_static(ctx, "./test_public");

	assert(csilk_is_async(ctx) == 1);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(csilk_get_status(ctx) == CSILK_STATUS_PARTIAL_CONTENT);
	const char* cr = csilk_get_response_header(ctx, "Content-Range");
	assert(cr != NULL && strncmp(cr, "bytes 0-4/", 10) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_static_range_first_5 passed\n");
}

void
test_static_range_middle()
{
	printf("Testing static file Range request (7-11)...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_set(ctx, "static_prefix", "/static");
	csilk_test_ctx_set_request(ctx, "GET", "/static/hello.txt");
	csilk_set_request_header(ctx, "Range", "bytes=7-11");

	csilk_static(ctx, "./test_public");

	assert(csilk_is_async(ctx) == 1);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(csilk_get_status(ctx) == CSILK_STATUS_PARTIAL_CONTENT);
	const char* cr = csilk_get_response_header(ctx, "Content-Range");
	assert(cr != NULL && strncmp(cr, "bytes 7-11/", 10) == 0);

	csilk_test_ctx_free(ctx);
	printf("test_static_range_middle passed\n");
}

void
test_static_range_invalid()
{
	printf("Testing static file invalid Range request...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();

	csilk_set(ctx, "static_prefix", "/static");
	csilk_test_ctx_set_request(ctx, "GET", "/static/hello.txt");
	csilk_set_request_header(ctx, "Range", "bytes=50-100");

	csilk_static(ctx, "./test_public");

	assert(csilk_is_async(ctx) == 1);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(csilk_get_status(ctx) == CSILK_STATUS_RANGE_NOT_SATISFIABLE);

	csilk_test_ctx_free(ctx);
	printf("test_static_range_invalid passed\n");
}

int
main()
{
	setup_test_file();
	test_static_serves_file();
	test_static_traversal_blocked();
	test_static_range_first_5();
	test_static_range_middle();
	test_static_range_invalid();

	remove("./test_public/hello.txt");
	rmdir("./test_public");
	return 0;
}
