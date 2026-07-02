#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_basic_form()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "name=John&age=30&city=NYC";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_set_request_header(c, "Content-Type", "application/x-www-form-urlencoded");

    csilk_parse_form_urlencoded(c);

    assert(strcmp(csilk_get_form_field(c, "name"), "John") == 0);
    assert(strcmp(csilk_get_form_field(c, "age"), "30") == 0);
    assert(strcmp(csilk_get_form_field(c, "city"), "NYC") == 0);
    assert(csilk_get_form_field(c, "missing") == nullptr);

    csilk_test_ctx_free(c);
    printf("test_basic_form passed\n");
}

static void
test_urlencoded_form()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "name=%48%65%6C%6C%6F&msg=hello+world&special=a%2Fb";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_set_request_header(c, "Content-Type", "application/x-www-form-urlencoded");

    csilk_parse_form_urlencoded(c);

    assert(strcmp(csilk_get_form_field(c, "name"), "Hello") == 0);
    assert(strcmp(csilk_get_form_field(c, "msg"), "hello world") == 0);
    assert(strcmp(csilk_get_form_field(c, "special"), "a/b") == 0);

    csilk_test_ctx_free(c);
    printf("test_urlencoded_form passed\n");
}

static void
test_empty_form()
{
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_parse_form_urlencoded(c);
    assert(csilk_get_form_field(c, "any") == nullptr);

    csilk_test_ctx_free(c);
    printf("test_empty_form passed\n");
}

static void
test_no_content_type()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "key=value";
    csilk_test_ctx_set_body(c, body, strlen(body));

    csilk_parse_form_urlencoded(c);
    assert(csilk_get_form_field(c, "key") == nullptr);

    csilk_test_ctx_free(c);
    printf("test_no_content_type passed\n");
}

static void
test_wrong_content_type()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "key=value";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_set_request_header(c, "Content-Type", "application/json");

    csilk_parse_form_urlencoded(c);
    assert(csilk_get_form_field(c, "key") == nullptr);

    csilk_test_ctx_free(c);
    printf("test_wrong_content_type passed\n");
}

static void
test_null_context()
{
    csilk_parse_form_urlencoded(nullptr);
    assert(csilk_get_form_field(nullptr, "key") == nullptr);
    printf("test_null_context passed\n");
}

static void
test_duplicate_keys()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "key=first&key=second";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_set_request_header(c, "Content-Type", "application/x-www-form-urlencoded");

    csilk_parse_form_urlencoded(c);

    const char* val = csilk_get_form_field(c, "key");
    assert(val != nullptr);
    assert(strcmp(val, "second") == 0);

    csilk_test_ctx_free(c);
    printf("test_duplicate_keys passed\n");
}

static void
test_empty_value()
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  body = "key=&other=val";
    csilk_test_ctx_set_body(c, body, strlen(body));
    csilk_set_request_header(c, "Content-Type", "application/x-www-form-urlencoded");

    csilk_parse_form_urlencoded(c);

    const char* val = csilk_get_form_field(c, "key");
    assert(val != nullptr);
    assert(strcmp(val, "") == 0);
    assert(strcmp(csilk_get_form_field(c, "other"), "val") == 0);

    csilk_test_ctx_free(c);
    printf("test_empty_value passed\n");
}

int
main()
{
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
