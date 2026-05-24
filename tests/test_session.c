#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_internal.h"

static void test_session_start() {
  csilk_session_init();

  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_session_start(&c);

  void* session = csilk_get(&c, "_session");
  assert(session != NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_session_start passed\n");
}

static void test_session_set_get() {
  csilk_session_init();

  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_session_start(&c);

  int val = 42;
  csilk_session_set(&c, "answer", &val);
  int* retrieved = csilk_session_get(&c, "answer");
  assert(retrieved != NULL);
  assert(*retrieved == 42);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_session_set_get passed\n");
}

static void test_session_get_missing() {
  csilk_session_init();

  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_session_start(&c);

  void* val = csilk_session_get(&c, "nonexistent");
  assert(val == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_session_get_missing passed\n");
}

static void test_session_overwrite() {
  csilk_session_init();

  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_session_start(&c);

  int val1 = 1;
  int val2 = 2;
  csilk_session_set(&c, "key", &val1);
  csilk_session_set(&c, "key", &val2);

  int* retrieved = csilk_session_get(&c, "key");
  assert(retrieved != NULL);
  assert(*retrieved == 2);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_session_overwrite passed\n");
}

static void test_session_null_safety() {
  csilk_session_start(NULL);
  csilk_session_set(NULL, "key", NULL);
  assert(csilk_session_get(NULL, "key") == NULL);
  csilk_session_destroy(NULL);
  printf("test_session_null_safety passed\n");
}

int main() {
  test_session_start();
  test_session_set_get();
  test_session_get_missing();
  test_session_overwrite();
  test_session_null_safety();
  printf("test_session: ALL PASSED\n");
  return 0;
}
