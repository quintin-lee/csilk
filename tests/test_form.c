#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

static void test_basic_form() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("name=John&age=30&city=NYC");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type",
                           "application/x-www-form-urlencoded");

  csilk_parse_form_urlencoded(&c);

  assert(strcmp(csilk_get_form_field(&c, "name"), "John") == 0);
  assert(strcmp(csilk_get_form_field(&c, "age"), "30") == 0);
  assert(strcmp(csilk_get_form_field(&c, "city"), "NYC") == 0);
  assert(csilk_get_form_field(&c, "missing") == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_basic_form passed\n");
}

static void test_urlencoded_form() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("name=%48%65%6C%6C%6F&msg=hello+world&special=a%2Fb");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type",
                           "application/x-www-form-urlencoded");

  csilk_parse_form_urlencoded(&c);

  assert(strcmp(csilk_get_form_field(&c, "name"), "Hello") == 0);
  assert(strcmp(csilk_get_form_field(&c, "msg"), "hello world") == 0);
  assert(strcmp(csilk_get_form_field(&c, "special"), "a/b") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_urlencoded_form passed\n");
}

static void test_empty_form() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_parse_form_urlencoded(&c);
  assert(csilk_get_form_field(&c, "any") == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_empty_form passed\n");
}

static void test_no_content_type() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("key=value");
  c.request.body_len = strlen(c.request.body);

  csilk_parse_form_urlencoded(&c);
  assert(csilk_get_form_field(&c, "key") == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_no_content_type passed\n");
}

static void test_wrong_content_type() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("key=value");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type", "application/json");

  csilk_parse_form_urlencoded(&c);
  assert(csilk_get_form_field(&c, "key") == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_wrong_content_type passed\n");
}

static void test_null_context() {
  csilk_parse_form_urlencoded(NULL);
  assert(csilk_get_form_field(NULL, "key") == NULL);
  printf("test_null_context passed\n");
}

static void test_duplicate_keys() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("key=first&key=second");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type",
                           "application/x-www-form-urlencoded");

  csilk_parse_form_urlencoded(&c);

  const char* val = csilk_get_form_field(&c, "key");
  assert(val != NULL);
  assert(strcmp(val, "second") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_duplicate_keys passed\n");
}

static void test_empty_value() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("key=&other=val");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type",
                           "application/x-www-form-urlencoded");

  csilk_parse_form_urlencoded(&c);

  const char* val = csilk_get_form_field(&c, "key");
  assert(val != NULL);
  assert(strcmp(val, "") == 0);
  assert(strcmp(csilk_get_form_field(&c, "other"), "val") == 0);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_empty_value passed\n");
}

int main() {
  test_basic_form();
  test_urlencoded_form();
  test_empty_form();
  test_no_content_type();
  test_wrong_content_type();
  test_null_context();
  test_duplicate_keys();
  test_empty_value();
  printf("test_form: ALL PASSED\n");
  return 0;
}
