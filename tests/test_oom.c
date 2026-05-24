/**
 * @file test_oom.c
 * @brief OOM (Out Of Memory) simulation tests.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

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

int main() {
  test_oom_arena();
  test_oom_context();
  printf("All OOM tests passed!\n");
  return 0;
}
