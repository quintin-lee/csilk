#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static void test_split_url_no_query() {
  printf("Testing csilk_split_url with no query...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/hello/world", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/hello/world") == 0);
  assert(query == NULL);
  free(path);
  printf("csilk_split_url no query passed!\n");
}

static void test_split_url_with_query() {
  printf("Testing csilk_split_url with query...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/search?q=test&page=1", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/search") == 0);
  assert(query != NULL);
  assert(strcmp(query, "q=test&page=1") == 0);
  free(path);
  free(query);
  printf("csilk_split_url with query passed!\n");
}

static void test_split_url_encoded_path() {
  printf("Testing csilk_split_url with encoded path...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/%48%65%6C%6C%6F", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/Hello") == 0);
  assert(query == NULL);
  free(path);
  printf("csilk_split_url encoded path passed!\n");
}

static void test_split_url_with_encoded_query() {
  printf("Testing csilk_split_url with encoded query...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/path?name=%48%65%6C%6C%6F", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/path") == 0);
  assert(query != NULL);
  assert(strcmp(query, "name=%48%65%6C%6C%6F") == 0);
  free(path);
  free(query);
  printf("csilk_split_url with encoded query passed!\n");
}

static void test_split_url_null() {
  printf("Testing csilk_split_url with NULL url...\n");
  char *path = (char*)0xdeadbeef, *query = (char*)0xdeadbeef;
  csilk_split_url(NULL, &path, &query);
  assert(path == NULL);
  assert(query == NULL);
  printf("csilk_split_url NULL passed!\n");
}

static void test_split_url_empty_path() {
  printf("Testing csilk_split_url empty path...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "") == 0);
  assert(query == NULL);
  free(path);
  printf("csilk_split_url empty path passed!\n");
}

static void test_split_url_query_only_question() {
  printf("Testing csilk_split_url with trailing '?'...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/path?", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/path") == 0);
  assert(query != NULL);
  assert(strcmp(query, "") == 0);
  free(path);
  free(query);
  printf("csilk_split_url trailing '?' passed!\n");
}

static void test_split_url_multiple_question() {
  printf("Testing csilk_split_url with multiple '?'...\n");
  char *path = NULL, *query = NULL;
  csilk_split_url("/a?b?c", &path, &query);
  assert(path != NULL);
  assert(strcmp(path, "/a") == 0);
  assert(query != NULL);
  assert(strcmp(query, "b?c") == 0);
  free(path);
  free(query);
  printf("csilk_split_url multiple '?' passed!\n");
}

int main() {
  test_split_url_no_query();
  test_split_url_with_query();
  test_split_url_encoded_path();
  test_split_url_with_encoded_query();
  test_split_url_null();
  test_split_url_empty_path();
  test_split_url_query_only_question();
  test_split_url_multiple_question();
  printf("test_url_ext: ALL PASSED\n");
  return 0;
}
