#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"

void test_get_header() {
  printf("Testing csilk_get_header...\n");
  csilk_ctx_t c = {0};

  csilk_header_t* h1 = malloc(sizeof(csilk_header_t));
  h1->key = strdup("Content-Type");
  h1->value = strdup("application/json");
  h1->next = NULL;

  c.request.headers = h1;

  const char* val = csilk_get_header(&c, "Content-Type");
  assert(val != NULL);
  assert(strcmp(val, "application/json") == 0);

  // Test case insensitivity
  val = csilk_get_header(&c, "content-type");
  assert(val != NULL);
  assert(strcmp(val, "application/json") == 0);

  val = csilk_get_header(&c, "X-Not-Found");
  assert(val == NULL);

  csilk_ctx_cleanup(&c);
  printf("csilk_get_header passed!\n");
}

void test_set_header() {
  printf("Testing csilk_set_header...\n");
  csilk_ctx_t c = {0};

  csilk_set_header(&c, "Server", "Csilk/0.1.0");
  assert(c.response.headers != NULL);
  assert(strcmp(c.response.headers->key, "Server") == 0);
  assert(strcmp(c.response.headers->value, "Csilk/0.1.0") == 0);

  // Update existing header
  csilk_set_header(&c, "Server", "Csilk/1.0.0");
  assert(strcmp(c.response.headers->value, "Csilk/1.0.0") == 0);

  // Add another header
  csilk_set_header(&c, "Content-Type", "text/plain");
  assert(c.response.headers->next != NULL);
  assert(strcmp(c.response.headers->next->key, "Content-Type") == 0);
  assert(strcmp(c.response.headers->next->value, "text/plain") == 0);

  csilk_ctx_cleanup(&c);
  printf("csilk_set_header passed!\n");
}

void test_add_header() {
  printf("Testing csilk_add_header...\n");
  csilk_ctx_t c = {0};

  csilk_add_header(&c, "Set-Cookie", "a=1");
  csilk_add_header(&c, "Set-Cookie", "b=2");
  csilk_add_header(&c, "X-Custom", "value");

  int cookie_count = 0;
  int custom_found = 0;
  csilk_header_t* h = c.response.headers;
  while (h) {
    if (strcmp(h->key, "Set-Cookie") == 0) cookie_count++;
    if (strcmp(h->key, "X-Custom") == 0 && strcmp(h->value, "value") == 0) custom_found = 1;
    h = h->next;
  }
  assert(cookie_count == 2);
  assert(custom_found == 1);

  csilk_ctx_cleanup(&c);
  printf("csilk_add_header passed!\n");
}

int main() {
  test_get_header();
  test_set_header();
  test_add_header();
  printf("All header tests passed!\n");
  return 0;
}
