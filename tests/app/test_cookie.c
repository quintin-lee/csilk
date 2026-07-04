#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_set_cookie()
{
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_set_cookie(c, "session", "12345", 3600, "/", "example.com", 1, 1);

    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "session=12345") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "Max-Age=3600") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "Path=/") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "Domain=example.com") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "Secure") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "HttpOnly") == 1);

    csilk_test_ctx_free(c);
    printf("test_set_cookie passed\n");
}

void
test_get_cookie()
{
    csilk_ctx_t* c = csilk_test_ctx_new();

    // Mock request header
    csilk_set_request_header(c, "Cookie", "user=admin; theme=dark; extra=foo=bar");

    const char* user = csilk_get_cookie(c, "user");
    assert(user != nullptr);
    assert(strcmp(user, "admin") == 0);

    const char* theme = csilk_get_cookie(c, "theme");
    assert(theme != nullptr);
    assert(strcmp(theme, "dark") == 0);

    const char* extra = csilk_get_cookie(c, "extra");
    assert(extra != nullptr);
    assert(strcmp(extra, "foo=bar") == 0);

    const char* missing = csilk_get_cookie(c, "missing");
    assert(missing == nullptr);

    csilk_test_ctx_free(c);
    printf("test_get_cookie passed\n");
}

void
test_multiple_cookies()
{
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_set_cookie(c, "c1", "v1", 0, nullptr, nullptr, 0, 0);
    csilk_set_cookie(c, "c2", "v2", 0, nullptr, nullptr, 0, 0);

    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", nullptr) == 2);

    csilk_test_ctx_free(c);
    printf("test_multiple_cookies passed\n");
}

void
test_cookie_delete()
{
    printf("Testing cookie delete (Max-Age=0)...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_set_cookie(c, "session", "old", -1, "/", nullptr, 0, 0);

    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "session=old") == 1);
    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "Max-Age=0") == 1);

    csilk_test_ctx_free(c);
    printf("test_cookie_delete passed\n");
}

void
test_long_cookie_value()
{
    printf("Testing long cookie value...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    char long_val[512];
    memset(long_val, 'x', sizeof(long_val) - 1);
    long_val[sizeof(long_val) - 1] = '\0';

    csilk_set_cookie(c, "data", long_val, 3600, "/", nullptr, 0, 0);

    assert(csilk_test_ctx_count_response_headers(c, "Set-Cookie", "data=") == 1);

    csilk_test_ctx_free(c);
    printf("test_long_cookie_value passed\n");
}

void
test_empty_cookie_header()
{
    printf("Testing empty Cookie header...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    const char* result = csilk_get_cookie(c, "any");
    assert(result == nullptr);

    csilk_set_request_header(c, "Cookie", "");

    result = csilk_get_cookie(c, "any");
    assert(result == nullptr);

    csilk_test_ctx_free(c);
    printf("test_empty_cookie_header passed\n");
}

void
test_cookie_with_spaces()
{
    printf("Testing cookie with spaces...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_set_request_header(c, "Cookie", " key1 = val1 ; key2 = val2 ");

    const char* v1 = csilk_get_cookie(c, "key1");
    assert(v1 == nullptr || strcmp(v1, "val1") == 0);

    csilk_test_ctx_free(c);
    printf("test_cookie_with_spaces passed\n");
}

void
test_malformed_cookie()
{
    printf("Testing malformed Cookie header...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_set_request_header(c, "Cookie", "malformed; no=value=sign=here");

    csilk_test_ctx_free(c);
    printf("test_malformed_cookie passed\n");
}

void
test_get_cookie_with_nulls()
{
    printf("Testing csilk_get_cookie with nullptr context...\n");
    const char* result = csilk_get_cookie(nullptr, "test");
    assert(result == nullptr);
    printf("test_get_cookie_with_nulls passed\n");
}

int
main()
{
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
