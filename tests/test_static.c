#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define TEST_DIR "test_dir_static"

static void
cleanup_test_dir(void)
{
	remove(TEST_DIR "/test.html");
	rmdir(TEST_DIR);
}

void
test_static_serves_file()
{
	cleanup_test_dir();

	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(csilk_ctx_t));
	ctx.arena = csilk_arena_new(1024);
	assert(ctx.arena != NULL);
	ctx.request.path = strdup("/test.html");
	assert(ctx.request.path != NULL);

	if (mkdir(TEST_DIR, 0777) != 0) {
		fprintf(stderr, "WARNING: mkdir failed, skipping test\n");
		csilk_ctx_cleanup(&ctx);
		return;
	}

	FILE* f = fopen(TEST_DIR "/test.html", "w");
	if (!f) {
		fprintf(stderr, "WARNING: fopen failed, skipping test\n");
		rmdir(TEST_DIR);
		csilk_ctx_cleanup(&ctx);
		return;
	}

	fprintf(f, "<html><body>Hello</body></html>");
	fclose(f);

	csilk_static(&ctx, TEST_DIR);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(ctx.response.status == CSILK_STATUS_OK);
	assert(ctx.file_fd >= 0);
	assert(ctx.file_size > 0);

	printf("test_static_serves_file: PASS\n");
	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	cleanup_test_dir();
}

void
test_static_traversal_blocked()
{
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(csilk_ctx_t));
	ctx.arena = csilk_arena_new(1024);
	assert(ctx.arena != NULL);
	ctx.request.path = strdup("/../../etc/passwd");
	assert(ctx.request.path != NULL);

	csilk_static(&ctx, ".");
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(ctx.response.status == CSILK_STATUS_FORBIDDEN ||
	       ctx.response.status == CSILK_STATUS_NOT_FOUND);

	printf("test_static_traversal_blocked: PASS\n");
	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
}

void
test_static_range_first_5()
{
	cleanup_test_dir();

	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(csilk_ctx_t));
	ctx.arena = csilk_arena_new(1024);
	assert(ctx.arena != NULL);
	ctx.request.path = strdup("/test.html");
	assert(ctx.request.path != NULL);

	if (mkdir(TEST_DIR, 0777) != 0) {
		csilk_ctx_cleanup(&ctx);
		return;
	}

	FILE* f = fopen(TEST_DIR "/test.html", "w");
	if (!f) {
		rmdir(TEST_DIR);
		csilk_ctx_cleanup(&ctx);
		return;
	}

	fprintf(f, "Hello World, this is a test file!");
	fclose(f);

	csilk_set_request_header(&ctx, "Range", "bytes=0-4");
	csilk_static(&ctx, TEST_DIR);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(ctx.response.status == CSILK_STATUS_PARTIAL_CONTENT);
	assert(ctx.file_fd >= 0);
	assert(ctx.file_size == 5);
	assert(ctx.file_offset == 0);

	printf("test_static_range_first_5: PASS\n");
	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	cleanup_test_dir();
}

void
test_static_range_middle()
{
	cleanup_test_dir();

	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(csilk_ctx_t));
	ctx.arena = csilk_arena_new(1024);
	assert(ctx.arena != NULL);
	ctx.request.path = strdup("/test.html");
	assert(ctx.request.path != NULL);

	if (mkdir(TEST_DIR, 0777) != 0) {
		csilk_ctx_cleanup(&ctx);
		return;
	}

	FILE* f = fopen(TEST_DIR "/test.html", "w");
	if (!f) {
		rmdir(TEST_DIR);
		csilk_ctx_cleanup(&ctx);
		return;
	}

	fprintf(f, "Hello World, this is a test file!");
	fclose(f);

	csilk_set_request_header(&ctx, "Range", "bytes=6-10");
	csilk_static(&ctx, TEST_DIR);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(ctx.response.status == CSILK_STATUS_PARTIAL_CONTENT);
	assert(ctx.file_fd >= 0);
	assert(ctx.file_size == 5);
	assert(ctx.file_offset == 6);

	printf("test_static_range_middle: PASS\n");
	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	cleanup_test_dir();
}

void
test_static_range_invalid()
{
	cleanup_test_dir();

	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(csilk_ctx_t));
	ctx.arena = csilk_arena_new(1024);
	assert(ctx.arena != NULL);
	ctx.request.path = strdup("/test.html");
	assert(ctx.request.path != NULL);

	if (mkdir(TEST_DIR, 0777) != 0) {
		csilk_ctx_cleanup(&ctx);
		return;
	}

	FILE* f = fopen(TEST_DIR "/test.html", "w");
	if (!f) {
		rmdir(TEST_DIR);
		csilk_ctx_cleanup(&ctx);
		return;
	}

	fprintf(f, "Hello World");
	fclose(f);

	csilk_set_request_header(&ctx, "Range", "bytes=999-1000");
	csilk_static(&ctx, TEST_DIR);
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(ctx.response.status == CSILK_STATUS_RANGE_NOT_SATISFIABLE);

	printf("test_static_range_invalid: PASS\n");
	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	cleanup_test_dir();
}

int
main()
{
	test_static_serves_file();
	test_static_traversal_blocked();
	test_static_range_first_5();
	test_static_range_middle();
	test_static_range_invalid();
	return 0;
}
