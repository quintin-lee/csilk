#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gin.h"

void mock_handler1(gin_ctx_t* c) { (void)c; }
void mock_handler2(gin_ctx_t* c) { (void)c; }

int main() {
  gin_router_t* r = gin_router_new();
  assert(r != NULL);

  gin_handler_t h1[] = {mock_handler1};
  gin_handler_t h2[] = {mock_handler2};

  gin_router_add(r, "GET", "/hello", h1, 1);
  gin_router_add(r, "POST", "/submit", h2, 1);

  gin_handler_t* matched;

  matched = gin_router_match(r, "GET", "/hello");
  assert(matched != NULL && matched[0] == mock_handler1);

  matched = gin_router_match(r, "POST", "/submit");
  assert(matched != NULL && matched[0] == mock_handler2);

  assert(gin_router_match(r, "GET", "/notfound") == NULL);
  assert(gin_router_match(r, "POST", "/hello") == NULL);

  gin_router_free(r);
  printf("test_router passed!\n");
  return 0;
}
