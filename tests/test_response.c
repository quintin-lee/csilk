/**
 * @file test_response.c
 * @brief Unit tests for response building functions.
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "csilk/core/ctx_types.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
	do {                                                                                       \
		tests_run++;                                                                       \
		printf("  FAIL: %s\n", msg);                                                       \
	} while (0)

static void
test_status_sets_code(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_status(c, 201);
	if (csilk_get_status(c) == 201) {
		PASS();
	} else {
		FAIL("status not set");
	}
	csilk_test_ctx_free(c);
}

static void
test_status_null_safe(void)
{
	csilk_status(nullptr, 500);
	PASS();
}

static void
test_string_sets_body(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_string(c, 200, "hello world");
	int ok = (csilk_get_status(c) == 200 && c->response.body &&
		  strcmp(c->response.body, "hello world") == 0 && c->response.body_len == 11);
	if (ok) {
		PASS();
	} else {
		FAIL("string body not set");
	}
	csilk_test_ctx_free(c);
}

static void
test_string_empty(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_string(c, 200, "");
	if (csilk_get_status(c) == 200 && c->response.body_len == 0) {
		PASS();
	} else {
		FAIL("empty string body");
	}
	csilk_test_ctx_free(c);
}

static void
test_json_content_type(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	cJSON* obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "key", "value");
	csilk_json(c, 200, obj);
	int ok =
	    (csilk_get_status(c) == 200) &&
	    (csilk_test_ctx_count_response_headers(c, "Content-Type", "application/json") >= 1);
	if (ok) {
		PASS();
	} else {
		FAIL("json Content-Type missing");
	}
	csilk_test_ctx_free(c);
}

static void
test_json_body_valid(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	cJSON* obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "msg", "hello");
	csilk_json(c, 201, obj);
	int ok = c->response.body && strstr(c->response.body, "\"msg\"") &&
		 strstr(c->response.body, "\"hello\"");
	if (ok) {
		PASS();
	} else {
		FAIL("json body invalid");
	}
	csilk_test_ctx_free(c);
}

static void
test_json_null_safe(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_json(c, 200, nullptr);
	cJSON* leak_test = cJSON_CreateObject();
	csilk_json(nullptr, 200, leak_test);
	cJSON_Delete(leak_test);
	PASS();
	csilk_test_ctx_free(c);
}

static void
test_json_error_format(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_json_error(c, 400, "bad input");
	int ok = csilk_get_status(c) == 400 && c->response.body &&
		 strstr(c->response.body, "\"error\"") && strstr(c->response.body, "bad input");
	if (ok) {
		PASS();
	} else {
		FAIL("json_error format");
	}
	csilk_test_ctx_free(c);
}

static void
test_json_error_empty(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_json_error(c, 500, "");
	if (csilk_get_status(c) == 500 && c->response.body) {
		PASS();
	} else {
		FAIL("json_error empty");
	}
	csilk_test_ctx_free(c);
}

static void
test_set_header_single(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set_header(c, "X-Custom", "value1");
	int count = csilk_test_ctx_count_response_headers(c, "X-Custom", nullptr);
	if (count == 1) {
		PASS();
	} else {
		FAIL("set_header count");
	}
	csilk_test_ctx_free(c);
}

static void
test_set_header_replaces(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_set_header(c, "X-R", "old");
	csilk_set_header(c, "X-R", "new");
	int count = csilk_test_ctx_count_response_headers(c, "X-R", nullptr);
	if (count == 1) {
		PASS();
	} else {
		FAIL("set_header replace");
	}
	csilk_test_ctx_free(c);
}

static void
test_add_header_multiple(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_add_header(c, "Set-Cookie", "a=1");
	csilk_add_header(c, "Set-Cookie", "b=2");
	int count = csilk_test_ctx_count_response_headers(c, "Set-Cookie", nullptr);
	if (count == 2) {
		PASS();
	} else {
		FAIL("add_header append");
	}
	csilk_test_ctx_free(c);
}

int
main(void)
{
	printf("=== Response Module Tests ===\n\n");

	printf("--- csilk_status ---\n");
	test_status_sets_code();
	test_status_null_safe();

	printf("\n--- csilk_string ---\n");
	test_string_sets_body();
	test_string_empty();

	printf("\n--- csilk_json ---\n");
	test_json_content_type();
	test_json_body_valid();
	test_json_null_safe();

	printf("\n--- csilk_json_error ---\n");
	test_json_error_format();
	test_json_error_empty();

	printf("\n--- csilk_set_header / csilk_add_header ---\n");
	test_set_header_single();
	test_set_header_replaces();
	test_add_header_multiple();

	printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
	return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
