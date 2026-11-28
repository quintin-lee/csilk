/**
 * @file workflow_debug.c
 * @brief Workflow debug HTTP endpoint registration.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/core/sync.h"
#include "csilk/workflow/dsl.h"
#include "core/internal/obs_registry.h"
#include "workflow_internal.h"

static csilk_once_t g_wf_debug_route_once = CSILK_ONCE_INIT;

static void
wf_register_debug_route_once(void)
{
    _csilk_obs_register_route(
        "workflow_debug", "GET", "/api/v1/workflows/debug", csilk_wf_debug_handler);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor)) static void
wf_debug_route_constructor(void)
{
    csilk_once(&g_wf_debug_route_once, wf_register_debug_route_once);
}
#endif

CSILK_INTERNAL void
_wf_register_debug_route(void)
{
    csilk_once(&g_wf_debug_route_once, wf_register_debug_route_once);
}

void
csilk_wf_debug_handler(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_set_status(c, 200);
    const char* ok_msg = "{\"status\":\"debug_ws_connected\"}";
    csilk_set_response_body(c, ok_msg, strlen(ok_msg), 0);
}
