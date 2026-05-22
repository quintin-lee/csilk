#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gin.h"

// Define auth failure helper (based on typical gin behavior)
void gin_abort_with_status(gin_ctx_t* c, int status) {
  gin_status(c, status);
  gin_abort(c);
}

// Helper to add request header
void add_request_header(gin_ctx_t* c, const char* key, const char* value) {
  gin_header_t* h = malloc(sizeof(gin_header_t));
  h->key = strdup(key);
  h->value = strdup(value);
  h->next = c->request.headers;
  c->request.headers = h;
}

// Minimal test setup
int validate_success(const char* token) { return strcmp(token, "secret") == 0; }

void handler(gin_ctx_t* c) {
  gin_auth_middleware(c, validate_success);
  if (c->aborted) return;
  gin_string(c, 200, "authorized");
}

int main() {
  gin_ctx_t c;
  memset(&c, 0, sizeof(c));

  // Test 1: No header
  handler(&c);
  if (c.response.status != 401) {
    printf("Test 1 Failed: Expected 401, got %d\n", c.response.status);
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
    return 1;
  }
  printf("Test 3 Passed: Correct token results in 200\n");

  gin_ctx_cleanup(&c);
  return 0;
}
