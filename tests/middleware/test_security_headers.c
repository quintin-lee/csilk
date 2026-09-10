#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_security_headers_sets_all()
{
    printf("Testing security headers middleware sets all headers...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_handler_t handlers[] = {nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_security_headers_middleware(ctx);

    const char* frame = csilk_get_response_header(ctx, "X-Frame-Options");
    assert(frame != nullptr && strcmp(frame, "DENY") == 0);

    const char* ctype = csilk_get_response_header(ctx, "X-Content-Type-Options");
    assert(ctype != nullptr && strcmp(ctype, "nosniff") == 0);

    const char* xss = csilk_get_response_header(ctx, "X-XSS-Protection");
    assert(xss != nullptr && strcmp(xss, "0") == 0);

    const char* referrer = csilk_get_response_header(ctx, "Referrer-Policy");
    assert(referrer != nullptr && strcmp(referrer, "strict-origin-when-cross-origin") == 0);

    csilk_test_ctx_free(ctx);
    printf("security headers set all passed!\n");
}

int
main()
{
    test_security_headers_sets_all();
    printf("test_security_headers: ALL PASSED\n");
    return 0;
}
