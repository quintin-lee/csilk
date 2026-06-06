#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_request_id()
{
	printf("Testing request ID generation...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	// Case 1: ID is empty, should be generated
	csilk_request_id_middleware(c);
	const char* rid1 = csilk_get_request_id(c);
	assert(rid1 != nullptr && rid1[0] != '\0');
	assert(strlen(rid1) == 36);

	// Case 2: ID is already present, should be preserved
	csilk_ctx_t* c2 = csilk_test_ctx_new();
	csilk_set_request_id(c2, "my-custom-id");
	csilk_request_id_middleware(c2);
	const char* rid2 = csilk_get_request_id(c2);
	assert(strcmp(rid2, "my-custom-id") == 0);

	csilk_test_ctx_free(c);
	csilk_test_ctx_free(c2);
	printf("test_request_id passed\n");
}

void
test_health_check()
{
	printf("Testing health check handler...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_health_check_handler(c);

	assert(csilk_get_status(c) == CSILK_STATUS_OK);
	size_t len;
	const char* body = csilk_get_response_body(c, &len);
	assert(body != nullptr);
	assert(strstr(body, "\"status\":\"up\"") != nullptr);

	csilk_test_ctx_free(c);
	printf("test_health_check passed\n");
}

int
main()
{
	test_request_id();
	test_health_check();
	printf("test_extra: ALL PASSED\n");
	return 0;
}
