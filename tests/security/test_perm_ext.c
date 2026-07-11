#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/perm.h"
#include "csilk/test/test.h"

// Mock Permission Driver
static int driver_eval_called = 0;
static int
mock_eval(csilk_ctx_t* c, const char* perm, const char* res)
{
    (void)c;
    driver_eval_called++;
    if (perm && strcmp(perm, "allow") == 0) {
        return 0; // 0 = allowed
    }
    return -1;    // non-zero = denied
}

static csilk_perm_driver_t mock_driver = {.name = "ext_mock", .check = mock_eval};

void
test_perm_require_allowed()
{
    printf("Testing csilk_perm_require (Allowed)...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_perm_register_driver("ext_mock", &mock_driver);
    csilk_perm_set_default("ext_mock");

    driver_eval_called = 0;
    csilk_perm_require(ctx, "allow", "any");

    assert(driver_eval_called == 1);
    assert(csilk_is_aborted(ctx) == 0);

    csilk_test_ctx_free(ctx);
    printf("test_perm_require_allowed passed\n");
}

void
test_perm_require_forbidden()
{
    printf("Testing csilk_perm_require (Forbidden)...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_perm_register_driver("ext_mock", &mock_driver);
    csilk_perm_set_default("ext_mock");

    driver_eval_called = 0;
    csilk_perm_require(ctx, "deny", "any");

    assert(driver_eval_called == 1);
    assert(csilk_is_aborted(ctx) == 1);
    assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);

    csilk_test_ctx_free(ctx);
    printf("test_perm_require_forbidden passed\n");
}

void
test_perm_auto_middleware_null()
{
    printf("Testing csilk_perm_auto_middleware with NULLs...\n");
    csilk_perm_auto_middleware(nullptr);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_perm_auto_middleware(ctx);
    assert(csilk_is_aborted(ctx) == 0);

    csilk_test_ctx_free(ctx);
    printf("test_perm_auto_middleware_null passed\n");
}

int
main()
{
    csilk_perm_init();
    test_perm_require_allowed();
    test_perm_require_forbidden();
    test_perm_auto_middleware_null();
    printf("test_perm_ext: ALL PASSED\n");
    return 0;
}
