/**
 * @file test_storage.c
 * @brief Tests for pluggable storage interface.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"

static int set_called = 0;
static int get_called = 0;
static int clear_called = 0;

static void mock_set(csilk_ctx_t* c, const char* key, void* value) {
  (void)c;
  (void)key;
  (void)value;
  set_called++;
}

static void* mock_get(csilk_ctx_t* c, const char* key) {
  (void)c;
  (void)key;
  get_called++;
  return (void*)0x42;
}

static void mock_clear(csilk_ctx_t* c) {
  (void)c;
  clear_called++;
}

static csilk_storage_driver_t mock_driver = {
    .set = mock_set, .get = mock_get, .clear = mock_clear};

int main() {
  printf("Testing pluggable storage interface...\n");

  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  // Default behavior
  csilk_set(&c, "foo", (void*)0x1);
  assert(csilk_get(&c, "foo") == (void*)0x1);
  assert(set_called == 0);
  assert(get_called == 0);

  // Plug in driver
  c.storage_driver = &mock_driver;

  csilk_set(&c, "bar", (void*)0x2);
  assert(set_called == 1);

  void* val = csilk_get(&c, "bar");
  assert(get_called == 1);
  assert(val == (void*)0x42);

  csilk_ctx_cleanup(&c);
  assert(clear_called == 1);

  csilk_arena_free(c.arena);
  printf("Pluggable storage interface tests passed!\n");
  return 0;
}
