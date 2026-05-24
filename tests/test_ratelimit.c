#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

static int handler_called = 0;

static void test_handler(csilk_ctx_t* c) {
  (void)c;
  handler_called++;
}

static void test_ratelimit_basic() {
  printf("Testing rate limit basic...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  ctx.handler_index = -1;

  csilk_handler_t handlers[] = {test_handler, NULL};
  ctx.handlers = handlers;

  handler_called = 0;
  csilk_rate_limit_middleware(&ctx, 100);
  assert(handler_called == 1);
  assert(ctx.aborted == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("Rate limit basic test passed!\n");
}

int main() {
  test_ratelimit_basic();
  printf("test_ratelimit: ALL PASSED\n");
  return 0;
}
