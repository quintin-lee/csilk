#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_redirect_basic()
{
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_redirect(c, CSILK_STATUS_FOUND, "/new-location");

	assert(csilk_get_status(c) == CSILK_STATUS_FOUND);
	assert(csilk_is_aborted(c) == 1);

	const char* loc = csilk_get_response_header(c, "Location");
	assert(loc != NULL);
	assert(strcmp(loc, "/new-location") == 0);

	csilk_test_ctx_free(c);
	printf("test_redirect_basic passed\n");
}

static void
test_redirect_simple()
{
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_redirect_simple(c, "/redirect-simple");

	assert(csilk_get_status(c) == CSILK_STATUS_FOUND);
	assert(csilk_is_aborted(c) == 1);

	const char* loc = csilk_get_response_header(c, "Location");
	assert(loc != NULL);
	assert(strcmp(loc, "/redirect-simple") == 0);

	csilk_test_ctx_free(c);
	printf("test_redirect_simple passed\n");
}

static void
test_redirect_status_codes()
{
	int codes[] = {
	    CSILK_STATUS_MOVED_PERMANENTLY,
	    CSILK_STATUS_FOUND,
	    CSILK_STATUS_TEMPORARY_REDIRECT,
	};

	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		csilk_ctx_t* c = csilk_test_ctx_new();

		csilk_redirect(c, codes[i], "/target");

		assert(csilk_get_status(c) == codes[i]);
		assert(csilk_is_aborted(c) == 1);

		const char* loc = csilk_get_response_header(c, "Location");
		assert(loc != NULL);
		assert(strcmp(loc, "/target") == 0);

		csilk_test_ctx_free(c);
	}
	printf("test_redirect_status_codes passed\n");
}

static void
test_redirect_null_safety()
{
	csilk_redirect(NULL, CSILK_STATUS_FOUND, "/nowhere");
	csilk_redirect_simple(NULL, "/nowhere");

	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_redirect(c, CSILK_STATUS_FOUND, NULL);
	assert(csilk_is_aborted(c) == 0);
	assert(csilk_get_status(c) == 0);

	csilk_test_ctx_free(c);
	printf("test_redirect_null_safety passed\n");
}

int
main()
{
	test_redirect_basic();
	test_redirect_simple();
	test_redirect_status_codes();
	test_redirect_null_safety();
	printf("test_redirect: ALL PASSED\n");
	return 0;
}
