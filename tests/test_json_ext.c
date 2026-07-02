/**
 * @file test_json_ext.c
 * @brief Extended tests for JSON response and reflection binding.
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
    } while (0)

/* ------------------------------------------------------------------ */

static void
test_json_roundtrip(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", 42);
    cJSON_AddStringToObject(obj, "name", "Alice");

    csilk_json(c, 200, obj);

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    int         ok_body = body && strstr(body, "\"id\"") && strstr(body, "42") &&
                          strstr(body, "\"name\"") && strstr(body, "Alice");
    int         ok_status = csilk_get_status(c) == 200;
    int ok_ct = csilk_test_ctx_count_response_headers(c, "Content-Type", "application/json") >= 1;

    if (ok_body && ok_status && ok_ct) {
        PASS();
    } else {
        FAIL("json roundtrip");
    }
    csilk_test_ctx_free(c);
}

static void
test_json_large_payload(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       arr = cJSON_CreateArray();
    for (int i = 0; i < 100; i++) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "text", "repeated text for size testing");
        cJSON_AddItemToArray(arr, item);
    }

    csilk_json(c, 200, arr);

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    if (body && len > 500) {
        PASS();
    } else {
        FAIL("json large payload");
    }
    csilk_test_ctx_free(c);
}

static void
test_json_nested_structure(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       root = cJSON_CreateObject();
    cJSON*       meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "version", "1.0");
    cJSON_AddItemToObject(root, "meta", meta);
    cJSON_AddStringToObject(root, "data", "nested");

    csilk_json(c, 200, root);

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    if (body && strstr(body, "\"version\"") && strstr(body, "\"data\"")) {
        PASS();
    } else {
        FAIL("json nested structure");
    }
    csilk_test_ctx_free(c);
}

/* ------------------------------------------------------------------ */

static void
test_json_error_status_preserved(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_json_error(c, 422, "validation failed");

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    int         ok = csilk_get_status(c) == 422 && body && strstr(body, "422") == nullptr;
    if (ok) {
        PASS();
    } else {
        FAIL("json_error status");
    }
    csilk_test_ctx_free(c);
}

static void
test_json_error_copy(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_json_error(c, 500, "test error message");
    csilk_json_error(c, 503, "another");

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    if (csilk_get_status(c) == 503 && body && strstr(body, "another")) {
        PASS();
    } else {
        FAIL("json_error replace");
    }
    csilk_test_ctx_free(c);
}

/* ------------------------------------------------------------------ */

static void
test_reflect_json_basic(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "hello", "world");
    csilk_json_error(c, 200, "ok test");
    cJSON_Delete(obj);

    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    if (body && strstr(body, "\"error\"") && strstr(body, "ok test")) {
        PASS();
    } else {
        FAIL("reflect json basic");
    }
    csilk_test_ctx_free(c);
}

static void
test_bind_json_bad_input(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_body(c, "not-json", 8);
    cJSON* result = csilk_bind_json(c);
    if (result == nullptr) {
        PASS();
    } else {
        cJSON_Delete(result);
        FAIL("bind_json should fail on bad input");
    }
    csilk_test_ctx_free(c);
}

static void
test_bind_json_null_ctx(void)
{
    cJSON* result = csilk_bind_json(nullptr);
    if (result == nullptr) {
        PASS();
    } else {
        cJSON_Delete(result);
        FAIL("bind_json null ctx");
    }
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== JSON Extended Tests ===\n\n");

    printf("--- csilk_json edge cases ---\n");
    test_json_roundtrip();
    test_json_large_payload();
    test_json_nested_structure();

    printf("\n--- csilk_json_error edge cases ---\n");
    test_json_error_status_preserved();
    test_json_error_copy();

    printf("\n--- Reflection JSON ---\n");
    test_reflect_json_basic();
    test_bind_json_bad_input();
    test_bind_json_null_ctx();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
