#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

static int count_headers(csilk_header_map_t* map, const char* key,
                         const char* value_contains) {
  int count = 0;
  for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
    csilk_header_t* h = map->buckets[i];
    while (h) {
      if (strcasecmp(h->key, key) == 0) {
        if (!value_contains || strstr(h->value, value_contains)) {
          count++;
        }
      }
      h = h->next;
    }
  }
  return count;
}

void test_set_cookie() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_cookie(&c, "session", "12345", 3600, "/", "example.com", 1, 1);

  assert(count_headers(&c.response.headers, "Set-Cookie", "session=12345") ==
         1);
  assert(count_headers(&c.response.headers, "Set-Cookie", "Max-Age=3600") == 1);
  assert(count_headers(&c.response.headers, "Set-Cookie", "Path=/") == 1);
  assert(count_headers(&c.response.headers, "Set-Cookie",
                       "Domain=example.com") == 1);
  assert(count_headers(&c.response.headers, "Set-Cookie", "Secure") == 1);
  assert(count_headers(&c.response.headers, "Set-Cookie", "HttpOnly") == 1);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_set_cookie passed\n");
}

void test_get_cookie() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  // Mock request header
  csilk_set_request_header(&c, "Cookie",
                           "user=admin; theme=dark; extra=foo=bar");

  const char* user = csilk_get_cookie(&c, "user");
  assert(user != NULL);
  assert(strcmp(user, "admin") == 0);

  const char* theme = csilk_get_cookie(&c, "theme");
  assert(theme != NULL);
  assert(strcmp(theme, "dark") == 0);

  const char* extra = csilk_get_cookie(&c, "extra");
  assert(extra != NULL);
  assert(strcmp(extra, "foo=bar") == 0);

  const char* missing = csilk_get_cookie(&c, "missing");
  assert(missing == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_get_cookie passed\n");
}

void test_multiple_cookies() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_cookie(&c, "c1", "v1", 0, NULL, NULL, 0, 0);
  csilk_set_cookie(&c, "c2", "v2", 0, NULL, NULL, 0, 0);

  assert(count_headers(&c.response.headers, "Set-Cookie", NULL) == 2);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_multiple_cookies passed\n");
}

void test_cookie_delete() {
  printf("Testing cookie delete (Max-Age=0)...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_cookie(&c, "session", "old", -1, "/", NULL, 0, 0);

  assert(count_headers(&c.response.headers, "Set-Cookie", "session=old") == 1);
  assert(count_headers(&c.response.headers, "Set-Cookie", "Max-Age=0") == 1);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_cookie_delete passed\n");
}

void test_long_cookie_value() {
  printf("Testing long cookie value...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  char long_val[512];
  memset(long_val, 'x', sizeof(long_val) - 1);
  long_val[sizeof(long_val) - 1] = '\0';

  csilk_set_cookie(&c, "data", long_val, 3600, "/", NULL, 0, 0);

  assert(count_headers(&c.response.headers, "Set-Cookie", "data=") == 1);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_long_cookie_value passed\n");
}

void test_empty_cookie_header() {
  printf("Testing empty Cookie header...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  const char* result = csilk_get_cookie(&c, "any");
  assert(result == NULL);

  csilk_set_request_header(&c, "Cookie", "");

  result = csilk_get_cookie(&c, "any");
  assert(result == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_empty_cookie_header passed\n");
}

void test_cookie_with_spaces() {
  printf("Testing cookie with spaces...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_request_header(&c, "Cookie", " key1 = val1 ; key2 = val2 ");

  const char* v1 = csilk_get_cookie(&c, "key1");
  assert(v1 == NULL || strcmp(v1, "val1") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_cookie_with_spaces passed\n");
}

void test_malformed_cookie() {
  printf("Testing malformed Cookie header...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_request_header(&c, "Cookie", "malformed; no=value=sign=here");

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_malformed_cookie passed\n");
}

void test_get_cookie_with_nulls() {
  printf("Testing csilk_get_cookie with NULL context...\n");
  const char* result = csilk_get_cookie(NULL, "test");
  assert(result == NULL);
  printf("test_get_cookie_with_nulls passed\n");
}

int main() {
  test_set_cookie();
  test_get_cookie();
  test_multiple_cookies();
  test_cookie_delete();
  test_long_cookie_value();
  test_empty_cookie_header();
  test_cookie_with_spaces();
  test_malformed_cookie();
  test_get_cookie_with_nulls();
  printf("All cookie tests passed!\n");
  return 0;
}
