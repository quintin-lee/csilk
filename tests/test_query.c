#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gin.h"

void test_split_url() {
  char *path, *query;

  gin_split_url("/api/ping?a=1", &path, &query);
  assert(strcmp(path, "/api/ping") == 0);
  assert(strcmp(query, "a=1") == 0);
  free(path);
  free(query);

  gin_split_url("/noquery", &path, &query);
  assert(strcmp(path, "/noquery") == 0);
  assert(query == NULL);
  free(path);
  free(query);

  gin_split_url("?onlyquery=test", &path, &query);
  assert(strcmp(path, "") == 0);
  assert(strcmp(query, "onlyquery=test") == 0);
  free(path);
  free(query);

  printf("test_split_url passed\n");
}

void test_parse_query() {
  gin_ctx_t ctx = {0};

  gin_parse_query(&ctx, "a=1&b=2&c=hello");

  const char* a = gin_get_query(&ctx, "a");
  const char* b = gin_get_query(&ctx, "b");
  const char* c = gin_get_query(&ctx, "c");
  const char* d = gin_get_query(&ctx, "d");

  assert(a != NULL && strcmp(a, "1") == 0);
  assert(b != NULL && strcmp(b, "2") == 0);
  assert(c != NULL && strcmp(c, "hello") == 0);
  assert(d == NULL);

  gin_ctx_cleanup(&ctx);
  printf("test_parse_query passed\n");
}

void test_empty_query() {
  gin_ctx_t ctx = {0};
  gin_parse_query(&ctx, "");
  assert(ctx.request.query_params == NULL);

  gin_parse_query(&ctx, "key_no_val");
  const char* val = gin_get_query(&ctx, "key_no_val");
  assert(val != NULL && strcmp(val, "") == 0);

  gin_ctx_cleanup(&ctx);
  printf("test_empty_query passed\n");
}

int main() {
  test_split_url();
  test_parse_query();
  test_empty_query();
  return 0;
}
