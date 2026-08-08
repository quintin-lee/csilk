#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/middleware/otlp_trace.h"

static void
test_apm_dashboard_route_registration(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);

    csilk_otlp_serve_apm_ui(app, "/admin/apm");

    csilk_app_free(app);
    printf("test_apm_dashboard_route_registration passed\n");
}

int
main(void)
{
    test_apm_dashboard_route_registration();
    printf("All test_apm_dashboard_route tests passed successfully!\n");
    return 0;
}
