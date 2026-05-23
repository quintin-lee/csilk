#include "csilk_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

void test_split_url() {
  char *path, *query;

  csilk_split_url("/api/ping?a=1", &path, &query);
  assert(strcmp(path, "/api/ping") == 0);
  assert(strcmp(query, "a=1") == 0);
  free(path);
  free(query);

  csilk_split_url("/noquery", &path, &query);
  assert(strcmp(path, "/noquery") == 0);
  assert(query == NULL);
  free(path);
  free(query);

  csilk_split_url("?onlyquery=test", &path, &query);
  assert(strcmp(path, "") == 0);
  assert(strcmp(query, "onlyquery=test") == 0);
  free(path);
  free(query);

  printf("test_split_url passed\n");
}

void test_parse_query() {
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);

  csilk_parse_query(&ctx, "a=1&b=2&c=hello");

  const char* a = csilk_get_query(&ctx, "a");
  const char* b = csilk_get_query(&ctx, "b");
  const char* c = csilk_get_query(&ctx, "c");
  const char* d = csilk_get_query(&ctx, "d");

  assert(a != NULL && strcmp(a, "1") == 0);
  assert(b != NULL && strcmp(b, "2") == 0);
  assert(c != NULL && strcmp(c, "hello") == 0);
  assert(d == NULL);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("test_parse_query passed\n");
}

void test_empty_query() {
  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);

  csilk_parse_query(&ctx, "");
  // query_params is a struct, no longer NULLable. 
  // We check if getting a key returns NULL.
  assert(csilk_get_query(&ctx, "any") == NULL);

  csilk_parse_query(&ctx, "key_no_val");
  const char* val = csilk_get_query(&ctx, "key_no_val");
  assert(val != NULL && strcmp(val, "") == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("test_empty_query passed\n");
}

void test_boundary_query() {
  char *path, *query;
  
  // NULL url
  csilk_split_url(NULL, &path, &query);
  assert(path == NULL);
  assert(query == NULL);

  csilk_ctx_t ctx = {0};
  ctx.arena = csilk_arena_new(1024);
  
  // NULL query string
  csilk_parse_query(&ctx, NULL);
  assert(csilk_get_query(&ctx, "any") == NULL);

  // Consecutive ampersands
  csilk_parse_query(&ctx, "a=1&&b=2&");
  assert(strcmp(csilk_get_query(&ctx, "a"), "1") == 0);
  assert(strcmp(csilk_get_query(&ctx, "b"), "2") == 0);

  // Empty values
  csilk_parse_query(&ctx, "c=&d");
  assert(strcmp(csilk_get_query(&ctx, "c"), "") == 0);
  assert(strcmp(csilk_get_query(&ctx, "d"), "") == 0);

  csilk_ctx_cleanup(&ctx);
  csilk_arena_free(ctx.arena);
  printf("test_boundary_query passed\n");
}

int main() {
  test_split_url();
  test_parse_query();
  test_empty_query();
  test_boundary_query();
  return 0;
}
