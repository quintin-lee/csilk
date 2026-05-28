#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static volatile int panic_caught = 0;

static void
handler_that_panics(csilk_ctx_t* c)
{
	csilk_panic(c);
}

static void
test_recovery_handler_catches_panic()
{
	printf("Testing recovery handler catches panic...\n");
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.arena = csilk_arena_new(1024);

	csilk_handler_t handlers[] = {handler_that_panics, NULL};
	ctx.handlers = handlers;
	ctx.handler_index = -1;

	csilk_recovery_handler(&ctx);

	assert(ctx.response.status == CSILK_STATUS_INTERNAL_SERVER_ERROR);
	assert(ctx.response.body != NULL);
	assert(strstr(ctx.response.body, "Internal Server Error") != NULL);
	assert(ctx.has_jump_buffer == 0);

	csilk_ctx_cleanup(&ctx);
	csilk_arena_free(ctx.arena);
	printf("Recovery handler caught panic passed!\n");
}

static int normal_handler_called = 0;

static void
normal_handler(csilk_ctx_t* c)
{
	normal_handler_called = 1;
}

static void
test_recovery_handler_normal_flow()
{
	printf("Testing recovery handler normal flow...\n");
	csilk_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.arena = csilk_arena_new(1024);

	normal_handler_called = 0;
	csilk_handler_t handlers[] = {normal_handler, NULL};
	ctx.handlers = handlers;
	ctx.handler_index = -1;

	csilk_recovery_handler(&ctx);

	assert(normal_handler_called == 1);
	assert(ctx.has_jump_buffer == 0);

	csilk_arena_free(ctx.arena);
	printf("Recovery handler normal flow passed!\n");
}

int
main()
{
	test_recovery_handler_catches_panic();
	test_recovery_handler_normal_flow();
	printf("test_recovery_ext: ALL PASSED\n");
	return 0;
}
