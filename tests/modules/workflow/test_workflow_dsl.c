#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/app.h"
#include "csilk/test/test.h"
#include "csilk/workflow/dsl.h"

static void
test_dsl_json_parsing(void)
{
    const char* dsl_json =
        "{\n"
        "  \"name\": \"test_pipeline\",\n"
        "  \"budget\": {\"max_tokens\": 4096},\n"
        "  \"nodes\": [\n"
        "    {\"id\": \"node1\", \"type\": \"ai_chat\", \"config\": {\"model\": "
        "\"gpt-4o-mini\"}},\n"
        "    {\"id\": \"node2\", \"type\": \"agent_react\", \"depends_on\": [\"node1\"]}\n"
        "  ]\n"
        "}";

    char        err_buf[256] = {0};
    csilk_wf_t* wf = csilk_wf_from_json_ext(dsl_json, err_buf, sizeof(err_buf));
    assert(wf != nullptr);

    char* exported = csilk_wf_to_json(wf);
    assert(exported != nullptr);
    assert(strstr(exported, "test_pipeline") != nullptr);

    free(exported);
    csilk_wf_free(wf);
    printf("test_dsl_json_parsing passed\n");
}

static void
test_obs_route_mounting(void)
{
    csilk_wf_manager_t* mgr = csilk_wf_manager_new();
    assert(mgr != nullptr);

    csilk_wf_t* wf = csilk_wf_new("obs_mount_test");
    assert(wf != nullptr);

    csilk_app_t* app = csilk_app_new(nullptr);
    assert(app != nullptr);
    assert(csilk_app_mount_route(app, "workflow_ui", "/ui") == 0);
    assert(csilk_app_mount_route(app, "workflow_debug", nullptr) == 0);
    assert(csilk_app_mount_route(app, "nonexistent_route", nullptr) == -1);

    csilk_ctx_t* ui_ctx = csilk_test_ctx_new();
    csilk_wf_ui_handler(ui_ctx);
    assert(csilk_get_status(ui_ctx) == 200);
    const char* ui_body = csilk_get_response_body(ui_ctx, nullptr);
    assert(ui_body != nullptr && strstr(ui_body, "Workflow") != nullptr);
    csilk_test_ctx_free(ui_ctx);

    csilk_ctx_t* dbg_ctx = csilk_test_ctx_new();
    csilk_wf_debug_handler(dbg_ctx);
    assert(csilk_get_status(dbg_ctx) == 200);
    const char* dbg_body = csilk_get_response_body(dbg_ctx, nullptr);
    assert(dbg_body != nullptr && strstr(dbg_body, "debug_ws_connected") != nullptr);
    csilk_test_ctx_free(dbg_ctx);

    csilk_app_free(app);
    csilk_wf_free(wf);
    csilk_wf_manager_free(mgr);
    printf("test_obs_route_mounting passed\n");
}

int
main(void)
{
    test_dsl_json_parsing();
    test_obs_route_mounting();
    printf("All test_workflow_dsl tests passed!\n");
    return 0;
}
