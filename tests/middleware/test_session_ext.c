#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_session_destroy()
{
    printf("Testing csilk_session_destroy...\n");
    csilk_session_init();

    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_session_start(c);
    int val = 123;
    csilk_session_set(c, "key", &val);

    csilk_session_destroy(c);
    assert(csilk_session_get(c, "key") == nullptr);

    csilk_test_ctx_free(c);
    printf("test_session_destroy passed\n");
}

static void
test_session_resume()
{
    printf("Testing session resume from cookie...\n");
    csilk_session_init();

    csilk_ctx_t* ctx1 = csilk_test_ctx_new();
    csilk_session_start(ctx1);
    int val = 999;
    csilk_session_set(ctx1, "data", &val);

    const char* cookie = csilk_get_response_header(ctx1, "Set-Cookie");
    assert(cookie != nullptr);
    char id[64];
    // Extract session ID from cookie: csilk_session=ID; ...
    sscanf(cookie, "csilk_session=%63[^;]", id);

    csilk_ctx_t* ctx2 = csilk_test_ctx_new();
    char         cookie_hdr[128];
    snprintf(cookie_hdr, sizeof(cookie_hdr), "csilk_session=%s", id);
    csilk_set_request_header(ctx2, "Cookie", cookie_hdr);

    csilk_session_start(ctx2);
    int* retrieved = csilk_session_get(ctx2, "data");
    assert(retrieved != nullptr);
    assert(*retrieved == 999);

    csilk_test_ctx_free(ctx1);
    csilk_test_ctx_free(ctx2);
    printf("test_session_resume passed\n");
}

static void
test_session_get_no_session()
{
    printf("Testing csilk_session_get without session...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(csilk_session_get(c, "key") == nullptr);
    csilk_test_ctx_free(c);
    printf("test_session_get_no_session passed\n");
}

static void
test_session_set_get_null()
{
    printf("Testing csilk_session set/get with NULLs...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_session_start(c);

    csilk_session_set(c, nullptr, (void*)0x1);
    csilk_session_set(c, "key", nullptr);
    assert(csilk_session_get(c, nullptr) == nullptr);

    csilk_test_ctx_free(c);
    printf("test_session_set_get_null passed\n");
}

int
main()
{
    test_session_destroy();
    test_session_resume();
    test_session_get_no_session();
    test_session_set_get_null();
    printf("test_session_ext: ALL PASSED\n");
    return 0;
}
