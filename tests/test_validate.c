#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/internal.h"

static void test_validate_required() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_valid_rule_t rules[] = {{"name", CSILK_VALID_REQUIRED, 0, 0, NULL},
                                {NULL, 0, 0, 0, NULL}};

  const char* err = csilk_validate(&c, rules);
  assert(err != NULL);
  assert(strcmp(err, "name") == 0);

  csilk_parse_query(&c, "name=John");
  err = csilk_validate(&c, rules);
  assert(err == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_validate_required passed\n");
}

static void test_validate_int() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_valid_rule_t rules[] = {
      {"age", CSILK_VALID_INT | CSILK_VALID_REQUIRED, 18, 150, NULL},
      {NULL, 0, 0, 0, NULL}};

  const char* err = csilk_validate(&c, rules);
  assert(err != NULL);
  assert(strcmp(err, "age") == 0);

  csilk_parse_query(&c, "age=abc");
  err = csilk_validate(&c, rules);
  assert(err != NULL);

  csilk_parse_query(&c, "age=10");
  err = csilk_validate(&c, rules);
  assert(err != NULL);

  csilk_parse_query(&c, "age=200");
  err = csilk_validate(&c, rules);
  assert(err != NULL);

  csilk_parse_query(&c, "age=25");
  err = csilk_validate(&c, rules);
  assert(err == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_validate_int passed\n");
}

static void test_validate_string_length() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_valid_rule_t rules[] = {
      {"name", CSILK_VALID_STRING | CSILK_VALID_REQUIRED, 2, 10, NULL},
      {NULL, 0, 0, 0, NULL}};

  csilk_parse_query(&c, "name=A");
  assert(csilk_validate(&c, rules) != NULL);

  csilk_parse_query(&c, "name=Hello");
  assert(csilk_validate(&c, rules) == NULL);

  csilk_parse_query(&c, "name=ThisIsTooLong");
  assert(csilk_validate(&c, rules) != NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_validate_string_length passed\n");
}

static void test_validate_email() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  csilk_valid_rule_t rules[] = {
      {"email", CSILK_VALID_EMAIL | CSILK_VALID_REQUIRED, 0, 0, NULL},
      {NULL, 0, 0, 0, NULL}};

  csilk_parse_query(&c, "email=invalid");
  assert(csilk_validate(&c, rules) != NULL);

  csilk_parse_query(&c, "email=user@example.com");
  assert(csilk_validate(&c, rules) == NULL);

  csilk_parse_query(&c, "email=@domain.com");
  assert(csilk_validate(&c, rules) != NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_validate_email passed\n");
}

static void test_validate_null_safety() {
  assert(csilk_validate(NULL, NULL) == NULL);

  csilk_valid_rule_t rules[] = {{"f", CSILK_VALID_REQUIRED, 0, 0, NULL},
                                {NULL, 0, 0, 0, NULL}};
  assert(csilk_validate(NULL, rules) == NULL);
  printf("test_validate_null_safety passed\n");
}

static void test_validate_source_form() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);
  c.request.body = strdup("field=fromform");
  c.request.body_len = strlen(c.request.body);
  csilk_set_request_header(&c, "Content-Type",
                           "application/x-www-form-urlencoded");

  csilk_valid_rule_t rules[] = {{"field", CSILK_VALID_REQUIRED, 0, 0, "form"},
                                {NULL, 0, 0, 0, NULL}};

  csilk_parse_form_urlencoded(&c);
  const char* err = csilk_validate(&c, rules);
  assert(err == NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_validate_source_form passed\n");
}

int main() {
  test_validate_required();
  test_validate_int();
  test_validate_string_length();
  test_validate_email();
  test_validate_null_safety();
  test_validate_source_form();
  printf("test_validate: ALL PASSED\n");
  return 0;
}
