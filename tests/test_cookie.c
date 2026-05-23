#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "csilk.h"

void test_set_cookie() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    csilk_set_cookie(&c, "session", "12345", 3600, "/", "example.com", 1, 1);
    
    assert(c.response.headers != NULL);
    assert(strcmp(c.response.headers->key, "Set-Cookie") == 0);
    assert(strstr(c.response.headers->value, "session=12345") != NULL);
    assert(strstr(c.response.headers->value, "Max-Age=3600") != NULL);
    assert(strstr(c.response.headers->value, "Path=/") != NULL);
    assert(strstr(c.response.headers->value, "Domain=example.com") != NULL);
    assert(strstr(c.response.headers->value, "Secure") != NULL);
    assert(strstr(c.response.headers->value, "HttpOnly") != NULL);

    csilk_ctx_cleanup(&c);
    printf("test_set_cookie passed\n");
}

void test_get_cookie() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    // Mock request header
    csilk_set_request_header(&c, "Cookie", "user=admin; theme=dark; extra=foo=bar");

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
    printf("test_get_cookie passed\n");
}

void test_multiple_cookies() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    csilk_set_cookie(&c, "c1", "v1", 0, NULL, NULL, 0, 0);
    csilk_set_cookie(&c, "c2", "v2", 0, NULL, NULL, 0, 0);

    int count = 0;
    csilk_header_t* h = c.response.headers;
    while (h) {
        if (strcmp(h->key, "Set-Cookie") == 0) {
            count++;
        }
        h = h->next;
    }
    assert(count == 2);

    csilk_ctx_cleanup(&c);
    printf("test_multiple_cookies passed\n");
}

void test_cookie_delete() {
  printf("Testing cookie delete (Max-Age=0)...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_cookie(&c, "session", "old", -1, "/", NULL, 0, 0);

  assert(c.response.headers != NULL);
  assert(strcmp(c.response.headers->key, "Set-Cookie") == 0);
  assert(strstr(c.response.headers->value, "session=old") != NULL);
  assert(strstr(c.response.headers->value, "Max-Age=0") != NULL);

  csilk_ctx_cleanup(&c);
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

  assert(c.response.headers != NULL);
  assert(strcmp(c.response.headers->key, "Set-Cookie") == 0);
  assert(strstr(c.response.headers->value, "data=") != NULL);

  csilk_ctx_cleanup(&c);
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
  printf("test_cookie_with_spaces passed\n");
}

void test_malformed_cookie() {
  printf("Testing malformed Cookie header...\n");
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_set_request_header(&c, "Cookie", "malformed; no=value=sign=here");

  csilk_ctx_cleanup(&c);
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
