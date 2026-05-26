/**
 * @file test_oom.c
 * @brief OOM (Out Of Memory) simulation tests.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

#ifndef TEST_OOM
#error "TEST_OOM must be defined for this test"
#endif

void test_oom_arena() {
  printf("Testing Arena OOM...\n");
  g_oom_fail_after = 0; /* Fail on first malloc */
  g_oom_count = 0;

  csilk_arena_t* arena = csilk_arena_new(1024);
  assert(arena == NULL);

  g_oom_fail_after = 1; /* Succeed on arena struct, fail on first chunk */
  g_oom_count = 0;
  arena = csilk_arena_new(1024);
  assert(arena != NULL);
  void* ptr = csilk_arena_alloc(arena, 10);
  assert(ptr == NULL);
  csilk_arena_free(arena);

  g_oom_fail_after = -1; /* Disable OOM */
}

void test_oom_context() {
  printf("Testing Context storage OOM...\n");
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  g_oom_fail_after = 0;
  g_oom_count = 0;
  csilk_set(&c, "test", (void*)0x1);
  assert(csilk_get(&c, "test") == NULL);

  g_oom_fail_after = -1;
  csilk_arena_free(c.arena);
}

void test_oom_header_map() {
  printf("Testing Header Map OOM...\n");
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.arena = csilk_arena_new(64); /* small arena to force chunk allocation */

  g_oom_fail_after = 1; /* Struct allocated, but header entry fails */
  g_oom_count = 0;
  csilk_set_header(&ctx, "Key", "Value");
  assert(csilk_get_header(&ctx, "Key") == NULL);

  g_oom_fail_after = -1;
  csilk_arena_free(ctx.arena);
}

void test_oom_ws_send() {
  printf("Testing WS send OOM...\n");
  csilk_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.arena = csilk_arena_new(1024);

  uv_loop_t* loop = uv_default_loop();
  uv_tcp_t client;
  uv_tcp_init(loop, &client);
  ctx._internal_client = &client;

  g_oom_fail_after = 0;
  g_oom_count = 0;
  csilk_ws_send(&ctx, (uint8_t*)"payload", 7, 1);
  /* Should not crash */

  g_oom_fail_after = -1;
  csilk_arena_free(ctx.arena);
}

int main() {
  test_oom_arena();
  test_oom_context();
  test_oom_header_map();
  test_oom_ws_send();
  printf("All OOM tests passed!\n");
  return 0;
}
