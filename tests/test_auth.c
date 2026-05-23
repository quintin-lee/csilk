#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

// Define auth failure helper (based on typical csilk behavior)
void csilk_abort_with_status(csilk_ctx_t* c, int status) {
  csilk_status(c, status);
  csilk_abort(c);
}

// Helper to add request header
void add_request_header(csilk_ctx_t* c, const char* key, const char* value) {
  csilk_set_request_header(c, key, value);
}

// Minimal test setup
int validate_success(const char* token) { return strcmp(token, "secret") == 0; }

void handler(csilk_ctx_t* c) {
  csilk_auth_middleware(c, validate_success);
  if (c->aborted) return;
  csilk_string(c, 200, "authorized");
}

int main() {
  csilk_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.arena = csilk_arena_new(1024);

  // Test 1: No header
  handler(&c);
  if (c.response.status != 401) {
    printf("Test 1 Failed: Expected 401, got %d\n", c.response.status);
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    return 1;
  }
  printf("Test 1 Passed: No header results in 401\n");

  // Test 2: Wrong header
  c.aborted = 0;
  c.response.status = 0;  // reset
  add_request_header(&c, "Authorization", "wrong");
  handler(&c);
  if (c.response.status != 401) {
    printf("Test 2 Failed: Expected 401, got %d\n", c.response.status);
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    return 1;
  }
  printf("Test 2 Passed: Wrong token results in 401\n");

  // Test 3: Correct header
  c.aborted = 0;
  c.response.status = 0;  // reset
  add_request_header(&c, "Authorization", "secret");
  handler(&c);
  if (c.response.status != 200) {
    printf("Test 3 Failed: Expected 200, got %d\n", c.response.status);
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    return 1;
  }
  printf("Test 3 Passed: Correct token results in 200\n");

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  return 0;
}
