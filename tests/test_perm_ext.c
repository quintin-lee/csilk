#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/drivers/perm.h"

static int test_check_called = 0;
static int test_check_result = 0;
static int test_check_permission = 0;

static int test_driver_check(csilk_ctx_t* c, const char* permission,
                             const char* resource) {
  test_check_called = 1;
  return test_check_result;
}

static csilk_perm_driver_t test_driver = {.check = test_driver_check};

static void test_perm_register_driver() {
  printf("Testing perm_register_driver...\n");
  assert(csilk_perm_register_driver(NULL, NULL) == -1);
  assert(csilk_perm_register_driver("test", NULL) == -1);
  assert(csilk_perm_register_driver(NULL, &test_driver) == -1);
  int ret = csilk_perm_register_driver("test_driver", &test_driver);
  assert(ret == 0);
  printf("perm_register_driver passed!\n");
}

static void test_perm_get_driver() {
  printf("Testing perm_get_driver...\n");
  assert(csilk_perm_get_driver(NULL) == NULL);
  csilk_perm_driver_t* d = csilk_perm_get_driver("nonexistent");
  assert(d == NULL);
  d = csilk_perm_get_driver("test_driver");
  assert(d != NULL);
  printf("perm_get_driver passed!\n");
}

static void test_perm_set_default() {
  printf("Testing perm_set_default...\n");
  assert(csilk_perm_set_default("nonexistent") == -1);
  assert(csilk_perm_set_default("test_driver") == 0);
  printf("perm_set_default passed!\n");
}

static void test_perm_check() {
  printf("Testing perm_check...\n");
  test_check_called = 0;
  test_check_result = 0;

  int ret = csilk_perm_check(NULL, "read", "resource");
  assert(ret == 0);

  assert(test_check_called == 1);

  test_check_result = -1;
  ret = csilk_perm_check(NULL, "write", "resource");
  assert(ret == -1);
  printf("perm_check passed!\n");
}

static void test_perm_require_allowed() {
  printf("Testing perm_require allowed...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  test_check_result = 0;

  csilk_perm_require(&ctx, "read", "doc");
  assert(ctx.aborted == 0);

  csilk_arena_free(ctx.arena);
  printf("perm_require allowed passed!\n");
}

static void test_perm_require_forbidden() {
  printf("Testing perm_require forbidden...\n");
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  test_check_result = -1;

  csilk_perm_require(&ctx, "write", "secret");
  assert(ctx.aborted == 1);
  assert(ctx.response.status == CSILK_STATUS_FORBIDDEN);

  csilk_arena_free(ctx.arena);
  printf("perm_require forbidden passed!\n");
}

static void test_perm_auto_middleware_null() {
  printf("Testing perm_auto_middleware NULL...\n");
  csilk_perm_auto_middleware(NULL);
  csilk_ctx_t ctx = {0};
  csilk_perm_auto_middleware(&ctx);
  printf("perm_auto_middleware NULL passed!\n");
}

int main() {
  csilk_perm_init();
  test_perm_register_driver();
  test_perm_get_driver();
  test_perm_set_default();
  test_perm_check();
  test_perm_require_allowed();
  test_perm_require_forbidden();
  test_perm_auto_middleware_null();
  printf("test_perm_ext: ALL PASSED\n");
  return 0;
}
