/**
 * @file test_response_envelope.c
 * @brief Unit tests for RESTful response envelopes and pagination helpers.
 * @copyright MIT License
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/json/json.h"
#include "csilk/core/ctx/response.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                           \
    } while (0)

static void
test_respond_ok_with_data(void)
{
    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* data = csilk_json_object();
    csilk_json_add_string(data, "name", "csilk");

    csilk_respond_ok(c, data);

    size_t      len = 0;
    const char* body = csilk_get_response_body(c, &len);
    int         status = csilk_get_status(c);

    if (status != 200) {
        FAIL("status must be 200");
    } else if (!body || strstr(body, "\"code\":0") == NULL ||
               strstr(body, "\"message\":\"ok\"") == NULL ||
               strstr(body, "\"name\":\"csilk\"") == NULL) {
        FAIL("body envelope missing code, message, or data");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);
}

static void
test_respond_ok_null(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_respond_ok_null(c);

    size_t      len = 0;
    const char* body = csilk_get_response_body(c, &len);
    int         status = csilk_get_status(c);

    if (status != 200) {
        FAIL("status must be 200");
    } else if (!body || strstr(body, "\"code\":0") == NULL ||
               strstr(body, "\"data\":null") == NULL) {
        FAIL("body must have code:0 and data:null");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);
}

static void
test_respond_fail_and_semantics(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_respond_fail(c, 400, 1002, "invalid parameter");

    size_t      len = 0;
    const char* body = csilk_get_response_body(c, &len);
    int         status = csilk_get_status(c);

    if (status != 400) {
        FAIL("status must be 400");
    } else if (!body || strstr(body, "\"code\":1002") == NULL ||
               strstr(body, "\"message\":\"invalid parameter\"") == NULL) {
        FAIL("body must contain code:1002 and message");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);

    /* Test semantic helper fallback */
    c = csilk_test_ctx_new();
    csilk_respond_unauthorized(c, 0, NULL);
    body = csilk_get_response_body(c, &len);
    status = csilk_get_status(c);

    if (status != 401) {
        FAIL("status must be 401");
    } else if (!body || strstr(body, "\"code\":401") == NULL ||
               strstr(body, "\"message\":\"Unauthorized\"") == NULL) {
        FAIL("body must fall back to 401 code and Unauthorized text");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);
}

static void
test_parse_pagination(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    /* No query params -> default 1 and 20 */
    int64_t page = 0;
    int64_t page_size = 0;
    csilk_parse_pagination(c, &page, &page_size, 20, 500);

    if (page != 1 || page_size != 20) {
        FAIL("default pagination should be page=1, page_size=20");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);

    /* Clamping test */
    c = csilk_test_ctx_new();
    csilk_parse_query(c, "page=-5&page_size=9999");
    csilk_parse_pagination(c, &page, &page_size, 20, 500);

    if (page != 1 || page_size != 500) {
        FAIL("page must clamp to 1 and page_size must clamp to max 500");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);
}

static void
test_respond_page_and_ex(void)
{
    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* arr = csilk_json_array();
    csilk_json_array_append(arr, csilk_json_string_new("item1"));

    csilk_respond_page(c, arr, 42, 2, 20);

    size_t      len = 0;
    const char* body = csilk_get_response_body(c, &len);

    if (!body || strstr(body, "\"items\":[\"item1\"]") == NULL ||
        strstr(body, "\"total\":42") == NULL || strstr(body, "\"page\":2") == NULL ||
        strstr(body, "\"page_size\":20") == NULL) {
        FAIL("default page response must contain items, total, page, page_size");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);

    /* Test custom key "list" via csilk_respond_page_ex */
    c = csilk_test_ctx_new();
    arr = csilk_json_array();
    csilk_json_array_append(arr, csilk_json_string_new("rowA"));
    csilk_respond_page_ex(c, "list", arr, 10, 1, 10);

    body = csilk_get_response_body(c, &len);
    if (!body || strstr(body, "\"list\":[\"rowA\"]") == NULL ||
        strstr(body, "\"total\":10") == NULL) {
        FAIL("custom key list response must contain list and total");
    } else {
        PASS();
    }
    csilk_test_ctx_free(c);
}

int
main(void)
{
    printf("=== RESTful Response Envelope Tests ===\n\n");

    test_respond_ok_with_data();
    test_respond_ok_null();
    test_respond_fail_and_semantics();
    test_parse_pagination();
    test_respond_page_and_ex();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
