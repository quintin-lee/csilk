#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/middleware/otlp_trace.h"
#include "csilk/test/test.h"

static void
test_apm_dashboard_route_registration(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);

    csilk_otlp_serve_apm_ui(app, "/admin/apm");

    csilk_app_free(app);
    printf("test_apm_dashboard_route_registration passed\n");
}

static void
test_apm_dashboard_handler_content(void)
{
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != nullptr);
    csilk_test_ctx_set_request(ctx, "GET", "/admin/apm");

    csilk_handler_t handlers[] = {csilk_otlp_apm_ui_handler, nullptr};
    csilk_test_ctx_set_handlers(ctx, handlers);

    csilk_otlp_apm_ui_handler(ctx);

    assert(csilk_get_status(ctx) == 200);
    const char* ct = csilk_get_response_header(ctx, "Content-Type");
    assert(ct != nullptr && strstr(ct, "text/html") != nullptr);
    size_t      body_len = 0;
    const char* body = csilk_get_response_body(ctx, &body_len);
    assert(body != nullptr && body_len > 0);
    assert(strstr(body, "csilk APM") != nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_apm_dashboard_handler_content passed\n");
}

int
main(void)
{
    test_apm_dashboard_route_registration();
    test_apm_dashboard_handler_content();
    printf("All test_apm_dashboard_route tests passed successfully!\n");
    return 0;
}
