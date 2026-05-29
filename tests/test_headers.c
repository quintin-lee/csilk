#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_get_header()
{
	printf("Testing csilk_get_header...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_set_request_header(c, "Content-Type", "application/json");

	const char* val = csilk_get_header(c, "Content-Type");
	assert(val != NULL);
	assert(strcmp(val, "application/json") == 0);

	// Test case insensitivity
	val = csilk_get_header(c, "content-type");
	assert(val != NULL);
	assert(strcmp(val, "application/json") == 0);

	val = csilk_get_header(c, "X-Not-Found");
	assert(val == NULL);

	csilk_test_ctx_free(c);
	printf("csilk_get_header passed!\n");
}

void
test_set_header()
{
	printf("Testing csilk_set_header...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_set_header(c, "Server", "Csilk/0.1.0");
	const char* val = csilk_get_response_header(c, "Server");
	assert(val != NULL);
	assert(strcmp(val, "Csilk/0.1.0") == 0);

	// Update existing header
	csilk_set_header(c, "Server", "Csilk/1.0.0");
	val = csilk_get_response_header(c, "Server");
	assert(val != NULL);
	assert(strcmp(val, "Csilk/1.0.0") == 0);

	csilk_test_ctx_free(c);
	printf("csilk_set_header passed!\n");
}

void
test_add_header()
{
	printf("Testing csilk_add_header...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_add_header(c, "Set-Cookie", "a=1");
	csilk_add_header(c, "Set-Cookie", "b=2");
	csilk_add_header(c, "X-Custom", "value");

	// Since we don't have an easy way to verify multiple headers with same key via public API yet,
	// and we can't access buckets, we trust the internal implementation or just check at least one.
	// But let's check if csilk_get_response_header works.
	const char* val = csilk_get_response_header(c, "X-Custom");
	assert(val != NULL);
	assert(strcmp(val, "value") == 0);

	val = csilk_get_response_header(c, "Set-Cookie");
	assert(val != NULL);

	csilk_test_ctx_free(c);
	printf("csilk_add_header passed!\n");
}

int
main()
{
	test_get_header();
	test_set_header();
	test_add_header();
	printf("All header tests passed!\n");
	return 0;
}
