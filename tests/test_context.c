#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

int counter = 0;

void
m1(csilk_ctx_t* c)
{
	counter++;
	csilk_next(c);
	counter++;
}

void
m2(csilk_ctx_t* c)
{
	counter++;
	csilk_next(c);
	counter++;
}

void
handler(csilk_ctx_t* c)
{
	counter++;
}

void
test_basic_chaining()
{
	counter = 0;
	csilk_handler_t handlers[] = {m1, m2, handler, nullptr};
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_handlers(c, handlers);
	csilk_next(c);
	assert(counter == 5); // m1++, m2++, handler++, m2++, m1++
	csilk_test_ctx_free(c);
	printf("test_basic_chaining passed\n");
}

void
m_abort(csilk_ctx_t* c)
{
	counter++;
	csilk_abort(c);
}

void
test_abort()
{
	counter = 0;
	csilk_handler_t handlers[] = {m_abort, handler, nullptr};
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_handlers(c, handlers);
	csilk_next(c);
	assert(counter == 1);
	csilk_test_ctx_free(c);
	printf("test_abort passed\n");
}

void
handler_resp(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "hello");
}

void
test_context_response()
{
	csilk_handler_t handlers[] = {handler_resp, nullptr};
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_handlers(c, handlers);
	csilk_next(c);
	assert(csilk_get_status(c) == CSILK_STATUS_OK);
	size_t body_len = 0;
	assert(csilk_get_response_body(c, &body_len) != nullptr);
	assert(body_len == 5);
	csilk_test_ctx_free(c);
	printf("test_context_response passed\n");
}

void
test_context_storage()
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	int val = 42;
	csilk_set(c, "test_key", &val);

	void* retrieved = csilk_get(c, "test_key");
	assert(retrieved != nullptr);
	assert(*(int*)retrieved == 42);

	// Overwrite
	int val2 = 100;
	csilk_set(c, "test_key", &val2);
	assert(*(int*)csilk_get(c, "test_key") == 100);

	csilk_test_ctx_free(c);
	printf("test_context_storage passed\n");
}

void
test_context_arena()
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_arena_t* arena = csilk_get_arena(c);

	char* s = csilk_arena_strdup(arena, "arena string");
	assert(strcmp(s, "arena string") == 0);

	csilk_test_ctx_free(c);
	printf("test_context_arena passed\n");
}

int
main()
{
	test_basic_chaining();
	test_abort();
	test_context_response();
	test_context_storage();
	test_context_arena();
	return 0;
}
