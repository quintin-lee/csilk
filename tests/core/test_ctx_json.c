/**
 * @file test_ctx_json.c
 * @brief Unit tests for ctx_json.c: csilk_bind_json, csilk_get_cookie, csilk_bind_reflect.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_bind_json_null_ctx(void)
{
    printf("test_bind_json_null_ctx...\n");
    assert(csilk_bind_json(NULL) == NULL);
    printf("  passed\n");
}
static void
test_bind_json_no_body(void)
{
    printf("test_bind_json_no_body...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(csilk_bind_json(c) == NULL);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_bind_json_invalid(void)
{
    printf("test_bind_json_invalid...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_body(c, "not-json", 8);
    assert(csilk_bind_json(c) == NULL);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_bind_json_valid(void)
{
    printf("test_bind_json_valid...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "{\"key\": \"value\"}";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_json_t* json = csilk_bind_json(c);
    assert(json != NULL);
    csilk_json_t* val = csilk_json_get(json, "key");
    assert(val != NULL);
    assert(strcmp(csilk_json_string_value(val), "value") == 0);
    csilk_json_free(json);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_bind_json_err_null_ctx(void)
{
    printf("test_bind_json_err_null_ctx...\n");
    const char* err = NULL;
    assert(csilk_bind_json_err(NULL, &err) == NULL);
    assert(err != NULL);
    printf("  passed\n");
}
static void
test_bind_json_err_no_body(void)
{
    printf("test_bind_json_err_no_body...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  err = NULL;
    assert(csilk_bind_json_err(c, &err) == NULL);
    assert(err != NULL);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_get_cookie_null_ctx(void)
{
    printf("test_get_cookie_null_ctx...\n");
    assert(csilk_get_cookie(NULL, "x") == NULL);
    printf("  passed\n");
}
static void
test_get_cookie_missing(void)
{
    printf("test_get_cookie_missing...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_set_request_header(c, "Cookie", "a=1");
    assert(csilk_get_cookie(c, "b") == NULL);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_get_cookie_found(void)
{
    printf("test_get_cookie_found...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_set_request_header(c, "Cookie", "user=john; token=abc");
    const char* u = csilk_get_cookie(c, "user");
    assert(u != NULL && strcmp(u, "john") == 0);
    const char* t = csilk_get_cookie(c, "token");
    assert(t != NULL && strcmp(t, "abc") == 0);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
static void
test_bind_reflect_null(void)
{
    printf("test_bind_reflect_null...\n");
    int d = 0;
    assert(csilk_bind_reflect(NULL, "X", &d) == 0);
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(csilk_bind_reflect(c, "X", NULL) == 0);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}
int
main(void)
{
    test_bind_json_null_ctx();
    test_bind_json_no_body();
    test_bind_json_invalid();
    test_bind_json_valid();
    test_bind_json_err_null_ctx();
    test_bind_json_err_no_body();
    test_get_cookie_null_ctx();
    test_get_cookie_missing();
    test_get_cookie_found();
    test_bind_reflect_null();
    printf("All test_ctx_json tests passed!\n");
    return 0;
}
