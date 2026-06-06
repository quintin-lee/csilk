#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_session_start()
{
	csilk_session_init();

	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_session_start(c);

	void* session = csilk_get(c, "_session");
	assert(session != nullptr);

	csilk_test_ctx_free(c);
	printf("test_session_start passed\n");
}

static void
test_session_set_get()
{
	csilk_session_init();

	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_session_start(c);

	int val = 42;
	csilk_session_set(c, "answer", &val);
	int* retrieved = csilk_session_get(c, "answer");
	assert(retrieved != nullptr);
	assert(*retrieved == 42);

	csilk_test_ctx_free(c);
	printf("test_session_set_get passed\n");
}

static void
test_session_get_missing()
{
	csilk_session_init();

	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_session_start(c);

	void* val = csilk_session_get(c, "nonexistent");
	assert(val == nullptr);

	csilk_test_ctx_free(c);
	printf("test_session_get_missing passed\n");
}

static void
test_session_overwrite()
{
	csilk_session_init();

	csilk_ctx_t* c = csilk_test_ctx_new();

	csilk_session_start(c);

	int val1 = 1;
	int val2 = 2;
	csilk_session_set(c, "key", &val1);
	csilk_session_set(c, "key", &val2);

	int* retrieved = csilk_session_get(c, "key");
	assert(retrieved != nullptr);
	assert(*retrieved == 2);

	csilk_test_ctx_free(c);
	printf("test_session_overwrite passed\n");
}

static void
test_session_null_safety()
{
	csilk_session_start(nullptr);
	csilk_session_set(nullptr, "key", nullptr);
	assert(csilk_session_get(nullptr, "key") == nullptr);
	csilk_session_destroy(nullptr);
	printf("test_session_null_safety passed\n");
}

int
main()
{
	test_session_start();
	test_session_set_get();
	test_session_get_missing();
	test_session_overwrite();
	test_session_null_safety();
	printf("test_session: ALL PASSED\n");
	return 0;
}
