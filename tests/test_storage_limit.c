#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int
main()
{
	printf("Testing context storage limit (CSILK_MAX_STORAGE = %d)...\n", CSILK_MAX_STORAGE);

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	// Set CSILK_MAX_STORAGE items
	for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
		char key[32];
		snprintf(key, sizeof(key), "key%d", i);
		csilk_set(ctx, key, (void*)(intptr_t)i);
	}

	// Attempt to set one more item
	csilk_set(ctx, "too_many", (void*)999);

	// Verify first CSILK_MAX_STORAGE items are present
	for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
		char key[32];
		snprintf(key, sizeof(key), "key%d", i);
		void* val = csilk_get(ctx, key);
		assert(val == (void*)(intptr_t)i);
	}

	// Verify the overflow item is NOT present
	void* val = csilk_get(ctx, "too_many");
	assert(val == NULL);

	// Verify we can still update existing items
	csilk_set(ctx, "key0", (void*)123);
	val = csilk_get(ctx, "key0");
	assert(val == (void*)123);

	csilk_test_ctx_free(ctx);
	printf("test_storage_limit: PASS\n");
	return 0;
}
