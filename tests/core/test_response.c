/**
 * @file test_response.c
 * @brief Unit tests for response building functions.
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/ctx/ctx_internal.h"
#include "core/internal/srv_impl.h"
#include "core/internal/srv_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/json/json.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int            tests_run = 0;
static int            tests_passed = 0;
static int            mock_h3_write_calls = 0;
static int            mock_h3_end_calls = 0;
static int            mock_h3_queue_calls = 0;
static int            mock_h3_writable_calls = 0;
static csilk_ctx_t*   mock_h3_last_ctx = nullptr;
static const uint8_t* mock_h3_last_data = nullptr;
static size_t         mock_h3_last_len = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
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
    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    int ok = (csilk_get_status(c) == 200 && body && strcmp(body, "hello world") == 0 && len == 11);
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
    size_t len;
    csilk_get_response_body(c, &len);
    if (csilk_get_status(c) == 200 && len == 0) {
        PASS();
    } else {
        FAIL("empty string body");
    }
    csilk_test_ctx_free(c);
}

static void
test_json_content_type(void)
{
    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "key", "value");
    csilk_json(c, 200, obj);
    int ok = (csilk_get_status(c) == 200) &&
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
    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* obj = csilk_json_object();
    csilk_json_add_string(obj, "msg", "hello");
    csilk_json(c, 201, obj);
    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    int         ok = body && strstr(body, "\"msg\"") && strstr(body, "\"hello\"");
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
    csilk_json_t* leak_test = csilk_json_object();
    csilk_json(nullptr, 200, leak_test);
    csilk_json_free(leak_test);
    PASS();
    csilk_test_ctx_free(c);
}

static void
test_json_error_format(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_json_error(c, 400, "bad input");
    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    int         ok = csilk_get_status(c) == 400 && body && strstr(body, "\"error\"") &&
             strstr(body, "bad input");
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
    size_t      len;
    const char* body = csilk_get_response_body(c, &len);
    if (csilk_get_status(c) == 500 && body) {
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

static int
mock_h3_write(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
    mock_h3_write_calls++;
    mock_h3_last_ctx = c;
    mock_h3_last_data = data;
    mock_h3_last_len = len;
    return 1;
}

static void
mock_h3_end(csilk_ctx_t* c)
{
    mock_h3_end_calls++;
    mock_h3_last_ctx = c;
}

static size_t
mock_h3_get_write_queue_size(csilk_ctx_t* c)
{
    mock_h3_queue_calls++;
    mock_h3_last_ctx = c;
    return 42;
}

static int
mock_h3_is_writable(csilk_ctx_t* c)
{
    mock_h3_writable_calls++;
    mock_h3_last_ctx = c;
    return 1;
}

static void
test_h3_ops_unregistered_fail_closed(void)
{
    const csilk_h3_response_ops_t* prev_ops = _csilk_get_h3_response_ops();
    _csilk_register_h3_response_ops(nullptr);

    csilk_ctx_t*   c = csilk_test_ctx_new();
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.protocol = CSILK_PROTO_HTTP3;
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    c->_internal_client = &client;

    const uint8_t payload[] = "h3";
    int           w1 = csilk_response_write(c, payload, sizeof(payload) - 1);
    size_t        q1 = csilk_response_get_write_queue_size(c);
    int           wr1 = csilk_response_is_writable(c);
    csilk_response_end(c);

    static const csilk_h3_response_ops_t empty_ops = {0};
    _csilk_register_h3_response_ops(&empty_ops);

    int    w2 = csilk_response_write(c, payload, sizeof(payload) - 1);
    size_t q2 = csilk_response_get_write_queue_size(c);
    int    wr2 = csilk_response_is_writable(c);
    csilk_response_end(c);

    _csilk_register_h3_response_ops(prev_ops);

    if (w1 == -1 && q1 == 0 && wr1 == 0 && w2 == -1 && q2 == 0 && wr2 == 0 &&
        c->response_started == 0) {
        PASS();
    } else {
        FAIL("H3 unregistered ops did not fail closed");
    }

    c->_internal_client = nullptr;
    csilk_test_ctx_free(c);
}

static void
test_h3_ops_dispatch(void)
{
    static const csilk_h3_response_ops_t mock_ops = {
        .write = mock_h3_write,
        .end = mock_h3_end,
        .get_write_queue_size = mock_h3_get_write_queue_size,
        .is_writable = mock_h3_is_writable,
    };

    const csilk_h3_response_ops_t* prev_ops = _csilk_get_h3_response_ops();
    mock_h3_write_calls = 0;
    mock_h3_end_calls = 0;
    mock_h3_queue_calls = 0;
    mock_h3_writable_calls = 0;
    mock_h3_last_ctx = nullptr;
    mock_h3_last_data = nullptr;
    mock_h3_last_len = 0;

    _csilk_register_h3_response_ops(&mock_ops);

    csilk_ctx_t*   c = csilk_test_ctx_new();
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.protocol = CSILK_PROTO_HTTP3;
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    c->_internal_client = &client;

    const uint8_t payload[] = "abc";
    int           w = csilk_response_write(c, payload, 3);
    size_t        q = csilk_response_get_write_queue_size(c);
    int           wr = csilk_response_is_writable(c);
    csilk_response_end(c);

    _csilk_register_h3_response_ops(prev_ops);

    int ok = (w == 1 && q == 42 && wr == 1 && mock_h3_write_calls == 1 &&
              mock_h3_queue_calls == 1 && mock_h3_writable_calls == 1 && mock_h3_end_calls == 1 &&
              mock_h3_last_ctx == c && mock_h3_last_data == payload && mock_h3_last_len == 3);
    if (ok) {
        PASS();
    } else {
        FAIL("H3 ops dispatch mismatch");
    }

    c->_internal_client = nullptr;
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

    printf("\n--- HTTP/3 response ops dispatch ---\n");
    test_h3_ops_unregistered_fail_closed();
    test_h3_ops_dispatch();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
