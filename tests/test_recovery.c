#include <stdio.h>
#include <assert.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int handler_run = 0;

void
panic_handler(csilk_ctx_t* c)
{
	handler_run |= 1;
	csilk_panic(c);
}

void
after_panic_handler(csilk_ctx_t* c)
{
	handler_run |= 2;
	csilk_string(c, CSILK_STATUS_OK, "OK");
}

void
normal_handler(csilk_ctx_t* c)
{
	handler_run |= 4;
	csilk_string(c, CSILK_STATUS_OK, "OK");
	csilk_next(c);
}

void
deferred_free_handler(csilk_ctx_t* c)
{
	handler_run = 8;
	csilk_panic(c);
}

static void
deferred_cleanup(void* arg)
{
	*(int*)arg = 42;
}

static void
test_normal_handler()
{
	printf("Testing recovery with normal handler...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_handler_t handlers[] = {csilk_recovery_handler, normal_handler, nullptr};
	csilk_test_ctx_set_handlers(c, handlers);

	handler_run = 0;
	csilk_next(c);

	assert(handler_run == 4);
	assert(csilk_get_status(c) == CSILK_STATUS_OK);

	csilk_test_ctx_free(c);
	printf("  passed\n");
}

static void
test_panic_recovery()
{
	printf("Testing panic recovery...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_handler_t handlers[] = {
	    csilk_recovery_handler, panic_handler, after_panic_handler, nullptr};
	csilk_test_ctx_set_handlers(c, handlers);

	handler_run = 0;
	csilk_next(c);

	assert(handler_run == 1); /* panic_handler ran, after_panic_handler skipped */
	assert(csilk_get_status(c) == CSILK_STATUS_INTERNAL_SERVER_ERROR);

	csilk_test_ctx_free(c);
	printf("  passed\n");
}

static void
test_deferred_cleanup_on_panic()
{
	printf("Testing deferred cleanup on panic...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_handler_t handlers[] = {csilk_recovery_handler, deferred_free_handler, nullptr};
	csilk_test_ctx_set_handlers(c, handlers);

	int flag = 0;
	csilk_ctx_defer(c, deferred_cleanup, &flag);

	handler_run = 0;
	csilk_next(c);

	assert(handler_run == 8);
	assert(flag == 42); /* deferred cleanup must have been called */
	assert(csilk_get_status(c) == CSILK_STATUS_INTERNAL_SERVER_ERROR);

	csilk_test_ctx_free(c);
	printf("  passed\n");
}

static void
test_mixed_handler_chain()
{
	printf("Testing recovery with mixed handler chain...\n");
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_handler_t handlers[] = {
	    csilk_recovery_handler, normal_handler, panic_handler, after_panic_handler, nullptr};
	csilk_test_ctx_set_handlers(c, handlers);

	handler_run = 0;
	csilk_next(c);

	/* normal_handler ran and completed (set status OK), then panic_handler
	 * panicked — recovery should still produce 500 */
	assert(handler_run == 5); /* 4 + 1 */
	assert(csilk_get_status(c) == CSILK_STATUS_INTERNAL_SERVER_ERROR);

	csilk_test_ctx_free(c);
	printf("  passed\n");
}

int
main()
{
	test_normal_handler();
	test_panic_recovery();
	test_deferred_cleanup_on_panic();
	test_mixed_handler_chain();
	printf("test_recovery: ALL PASSED\n");
	return 0;
}
