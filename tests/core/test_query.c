#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void
test_split_url()
{
    char *path, *query;

    csilk_split_url("/api/ping?a=1", &path, &query);
    assert(strcmp(path, "/api/ping") == 0);
    assert(strcmp(query, "a=1") == 0);
    free(path);
    free(query);

    csilk_split_url("/noquery", &path, &query);
    assert(strcmp(path, "/noquery") == 0);
    assert(query == nullptr);
    free(path);
    free(query);

    csilk_split_url("?onlyquery=test", &path, &query);
    assert(strcmp(path, "") == 0);
    assert(strcmp(query, "onlyquery=test") == 0);
    free(path);
    free(query);

    printf("test_split_url passed\n");
}

void
test_parse_query()
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_parse_query(ctx, "a=1&b=2&c=hello");

    const char* a = csilk_get_query(ctx, "a");
    const char* b = csilk_get_query(ctx, "b");
    const char* c = csilk_get_query(ctx, "c");
    const char* d = csilk_get_query(ctx, "d");

    assert(a != nullptr && strcmp(a, "1") == 0);
    assert(b != nullptr && strcmp(b, "2") == 0);
    assert(c != nullptr && strcmp(c, "hello") == 0);
    assert(d == nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_parse_query passed\n");
}

void
test_empty_query()
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_parse_query(ctx, "");
    assert(csilk_get_query(ctx, "any") == nullptr);

    csilk_parse_query(ctx, "key_no_val");
    const char* val = csilk_get_query(ctx, "key_no_val");
    assert(val != nullptr && strcmp(val, "") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_empty_query passed\n");
}

void
test_boundary_query()
{
    char *path, *query;

    // nullptr url
    csilk_split_url(nullptr, &path, &query);
    assert(path == nullptr);
    assert(query == nullptr);

    csilk_ctx_t* ctx = csilk_test_ctx_new();

    // nullptr query string
    csilk_parse_query(ctx, nullptr);
    assert(csilk_get_query(ctx, "any") == nullptr);

    // Consecutive ampersands
    csilk_parse_query(ctx, "a=1&&b=2&");
    assert(strcmp(csilk_get_query(ctx, "a"), "1") == 0);
    assert(strcmp(csilk_get_query(ctx, "b"), "2") == 0);

    // Empty values
    csilk_parse_query(ctx, "c=&d");
    assert(strcmp(csilk_get_query(ctx, "c"), "") == 0);
    assert(strcmp(csilk_get_query(ctx, "d"), "") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_boundary_query passed\n");
}

int
main()
{
    test_split_url();
    test_parse_query();
    test_empty_query();
    test_boundary_query();
    return 0;
}
