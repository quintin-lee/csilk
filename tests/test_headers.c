#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gin.h"

void test_get_header() {
  printf("Testing gin_get_header...\n");
  gin_ctx_t c = {0};

  gin_header_t* h1 = malloc(sizeof(gin_header_t));
  h1->key = strdup("Content-Type");
  h1->value = strdup("application/json");
  h1->next = NULL;

  c.request.headers = h1;

  const char* val = gin_get_header(&c, "Content-Type");
  assert(val != NULL);
  assert(strcmp(val, "application/json") == 0);

  // Test case insensitivity
  val = gin_get_header(&c, "content-type");
  assert(val != NULL);
  assert(strcmp(val, "application/json") == 0);

  val = gin_get_header(&c, "X-Not-Found");
  assert(val == NULL);

  gin_ctx_cleanup(&c);
  printf("gin_get_header passed!\n");
}

void test_set_header() {
  printf("Testing gin_set_header...\n");
  gin_ctx_t c = {0};

  gin_set_header(&c, "Server", "Gin/0.1.0");
  assert(c.response.headers != NULL);
  assert(strcmp(c.response.headers->key, "Server") == 0);
  assert(strcmp(c.response.headers->value, "Gin/0.1.0") == 0);

  // Update existing header
  gin_set_header(&c, "Server", "Gin/1.0.0");
  assert(strcmp(c.response.headers->value, "Gin/1.0.0") == 0);

  // Add another header
  gin_set_header(&c, "Content-Type", "text/plain");
  assert(c.response.headers->next != NULL);
  assert(strcmp(c.response.headers->next->key, "Content-Type") == 0);
  assert(strcmp(c.response.headers->next->value, "text/plain") == 0);

  gin_ctx_cleanup(&c);
  printf("gin_set_header passed!\n");
}

int main() {
  test_get_header();
  test_set_header();
  printf("All header tests passed!\n");
  return 0;
}
