#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_get_client_ip_null_ctx()
{
	printf("Testing csilk_get_client_ip with NULL context...\n");
	const char* ip = csilk_get_client_ip(NULL);
	assert(ip == NULL);
	printf("csilk_get_client_ip NULL context passed!\n");
}

void
test_get_client_ip_no_client()
{
	printf("Testing csilk_get_client_ip with no internal client...\n");
	csilk_ctx_t* ctx = csilk_test_ctx_new();
	const char* ip = csilk_get_client_ip(ctx);
	assert(ip == NULL);
	csilk_test_ctx_free(ctx);
	printf("csilk_get_client_ip no client passed!\n");
}

void
test_get_client_ip_no_connection()
{
	printf("Testing csilk_get_client_ip with no active connection...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	/* _internal_client is NULL, so IP should be NULL */
	const char* ip = csilk_get_client_ip(ctx);
	assert(ip == NULL);

	csilk_test_ctx_free(ctx);
	printf("csilk_get_client_ip no connection passed!\n");
}

int
main()
{
	test_get_client_ip_null_ctx();
	test_get_client_ip_no_client();
	test_get_client_ip_no_connection();
	printf("All client IP tests passed!\n");
	return 0;
}
