#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

// Minimal test setup
int
validate_success(const char* token)
{
    return strcmp(token, "secret") == 0;
}

void
handler(csilk_ctx_t* c)
{
    csilk_auth_middleware(c, validate_success);
    if (csilk_is_aborted(c)) {
        return;
    }
    csilk_string(c, CSILK_STATUS_OK, "authorized");
}

int
main()
{
    csilk_ctx_t* c;

    // Test 1: No header
    c = csilk_test_ctx_new();
    handler(c);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);
    assert(csilk_is_aborted(c) == 1);
    csilk_test_ctx_free(c);
    printf("Test 1 Passed: No header results in 401\n");

    // Test 2: Wrong header
    c = csilk_test_ctx_new();
    csilk_set_request_header(c, "Authorization", "wrong");
    handler(c);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);
    assert(csilk_is_aborted(c) == 1);
    csilk_test_ctx_free(c);
    printf("Test 2 Passed: Wrong token results in 401\n");

    // Test 3: Correct header
    c = csilk_test_ctx_new();
    csilk_set_request_header(c, "Authorization", "secret");
    handler(c);
    assert(csilk_get_status(c) == CSILK_STATUS_OK);
    assert(csilk_is_aborted(c) == 0);
    csilk_test_ctx_free(c);
    printf("Test 3 Passed: Correct token results in 200\n");

    printf("test_auth: ALL PASSED\n");
    return 0;
}
