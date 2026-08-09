#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json.h"
#include "csilk/app/workflow_dsl.h"
#include "workflow_internal.h"

static void
workflow_debug_ws_handler(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_set_status(c, 200);
    const char* ok_msg = "{\"status\":\"debug_ws_connected\"}";
    csilk_set_response_body(c, ok_msg, strlen(ok_msg), 0);
}

int
csilk_wf_manager_enable_debug_server_impl(csilk_wf_manager_t* mgr,
                                          csilk_app_t*        app,
                                          const char*         route_path)
{
    if (!mgr || !app) {
        return -1;
    }

    const char* path = (route_path && *route_path) ? route_path : "/api/v1/workflows/debug";
    csilk_app_add_route(app, "GET", path, workflow_debug_ws_handler);
    return 0;
}
