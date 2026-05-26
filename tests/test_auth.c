#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

// Helper to add request header
void add_request_header(csilk_ctx_t* c, const char* key, const char* value) {
  csilk_set_request_header(c, key, value);
}

// Minimal test setup
int validate_success(const char* token) { return strcmp(token, "secret") == 0; }

void handler(csilk_ctx_t* c) {
  csilk_auth_middleware(c, validate_success);
  if (csilk_is_aborted(c)) return;
  csilk_string(c, CSILK_STATUS_OK, "authorized");
}

int main() {
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  csilk_arena_t* arena = csilk_arena_new(1024);
  /* We need a hack here because we're manually setting up ctx without
     csilk_server. In a real opaque world, we'd have csilk_ctx_new(). For now,
     we'll keep context_internal.h for tests that manual set up ctx. */
#include "csilk/core/context_internal.h"
  c.arena = arena;

  // Test 1: No header
  handler(&c);
  assert(csilk_get_status(&c) == CSILK_STATUS_UNAUTHORIZED);
  assert(csilk_is_aborted(&c) == 1);
  printf("Test 1 Passed: No header results in 401\n");

  // Test 2: Wrong header
  csilk_ctx_cleanup(&c);
  c.arena = arena;  // Restore arena after cleanup
  add_request_header(&c, "Authorization", "wrong");
  handler(&c);
  assert(csilk_get_status(&c) == CSILK_STATUS_UNAUTHORIZED);
  assert(csilk_is_aborted(&c) == 1);
  printf("Test 2 Passed: Wrong token results in 401\n");

  // Test 3: Correct header
  csilk_ctx_cleanup(&c);
  c.arena = arena;
  add_request_header(&c, "Authorization", "secret");
  handler(&c);
  assert(csilk_get_status(&c) == CSILK_STATUS_OK);
  assert(csilk_is_aborted(&c) == 0);
  printf("Test 3 Passed: Correct token results in 200\n");

  csilk_ctx_cleanup(&c);
  csilk_arena_free(arena);
  printf("test_auth: ALL PASSED\n");
  return 0;
}
