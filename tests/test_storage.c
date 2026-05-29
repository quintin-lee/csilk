#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int
main()
{
	printf("Testing context key-value storage...\n");

	csilk_ctx_t* c = csilk_test_ctx_new();

	int val1 = 123;
	csilk_set(c, "key1", &val1);

	char* val2 = "hello";
	csilk_set(c, "key2", val2);

	int* r1 = (int*)csilk_get(c, "key1");
	assert(r1 != NULL && *r1 == 123);

	char* r2 = (char*)csilk_get(c, "key2");
	assert(r2 != NULL && strcmp(r2, "hello") == 0);

	// Test update
	int val3 = 456;
	csilk_set(c, "key1", &val3);
	r1 = (int*)csilk_get(c, "key1");
	assert(r1 != NULL && *r1 == 456);

	// Test missing
	assert(csilk_get(c, "key3") == NULL);

	// Test overwrite with NULL
	csilk_set(c, "key2", NULL);
	assert(csilk_get(c, "key2") == NULL);

	csilk_test_ctx_free(c);
	printf("test_storage: PASS\n");
	return 0;
}
