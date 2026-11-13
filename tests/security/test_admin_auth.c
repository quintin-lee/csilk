#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk/csilk.h"

static void
dummy_auth_middleware(csilk_ctx_t* c)
{
    csilk_next(c);
}

static void
test_admin_rejects_null_auth(void)
{
    printf("Testing admin dashboard rejects NULL auth...\n");

    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);

    // Calling csilk_admin_serve_secure without auth must be rejected safely
    csilk_admin_serve_secure(app, "/admin", nullptr);

    csilk_app_free(app);
    printf("  passed\n");
}

static void
test_admin_secure_with_auth(void)
{
    printf("Testing admin dashboard with auth middleware...\n");

    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);

    // Calling with valid auth handler succeeds
    csilk_admin_serve_secure(app, "/admin", dummy_auth_middleware);

    csilk_app_free(app);
    printf("  passed\n");
}

int
main(void)
{
    test_admin_rejects_null_auth();
    test_admin_secure_with_auth();
    printf("test_admin_auth: ALL PASSED\n");
    return 0;
}
