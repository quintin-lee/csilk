#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"

static int g_fallback_ran = 0;
static int g_done = 0;

/* --- Handlers --- */

csilk_data_t*
invalid_json_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    printf("[Producer] Returning JSON missing 'required' field\n");
    return csilk_wf_data_new(
        ctx, "application/json", csilk_wf_strdup(ctx, "{\"wrong\": \"field\"}"));
}

csilk_data_t*
schema_fallback_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    printf("[Fallback] Executing due to schema validation failure\n");
    g_fallback_ran = 1;
    return nullptr;
}

void
on_schema_complete(csilk_data_t* result)
{
    (void)result;
    g_done = 1;
}

void
test_workflow_schema_validation()
{
    printf("Testing JSON Schema output validation...\n");
    g_fallback_ran = 0;
    g_done = 0;

    csilk_wf_t* wf = csilk_wf_new("schema_wf");

    csilk_wf_node_t* n1 = csilk_wf_add(wf, "invalid_node", invalid_json_handler, nullptr);

    // Require "result" field
    csilk_wf_node_set_schema(n1, "{\"required\": [\"result\"]}");

    csilk_wf_node_t* nf = csilk_wf_add(wf, "fallback", schema_fallback_handler, nullptr);
    csilk_wf_on_error(n1, nf);

    csilk_wf_node_set_entry(n1, 1);

    csilk_wf_run(wf, nullptr, on_schema_complete);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(g_fallback_ran == 1);
    assert(g_done == 1);

    csilk_wf_free(wf);
    printf("test_workflow_schema_validation: PASS\n");
}

int
main()
{
    test_workflow_schema_validation();
    return 0;
}
