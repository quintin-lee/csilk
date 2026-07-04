#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"

static int g_n1_count = 0;
static int g_done = 0;

csilk_data_t*
n1_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    g_n1_count++;
    printf("N1 executing (count: %d)\n", g_n1_count);
    return nullptr;
}

csilk_data_t*
n2_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    printf("N2 (Check) executing\n");
    if (g_n1_count < 3) {
        printf("N2: fail -> loop back\n");
        return csilk_wf_data_new(ctx, "fail", nullptr);
    } else {
        printf("N2: pass -> proceed\n");
        return csilk_wf_data_new(ctx, "pass", nullptr);
    }
}

csilk_data_t*
n3_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    printf("N3 executing\n");
    g_done = 1;
    return nullptr;
}

void
on_agent_complete(csilk_data_t* result)
{
    (void)result;
    printf("Agent Workflow complete!\n");
}

void
test_workflow_agentic()
{
    printf("Testing agentic workflow loop with Arena...\n");
    g_n1_count = 0;
    g_done = 0;

    csilk_wf_t* wf = csilk_wf_new("agentic_wf");

    csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", n1_handler, nullptr);
    csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", n2_handler, nullptr);
    csilk_wf_node_t* n3 = csilk_wf_add(wf, "n3", n3_handler, nullptr);

    csilk_wf_node_set_entry(n1, 1);

    csilk_wf_bind(n1, n2);
    csilk_wf_on_loop(n2, "fail", n1);
    csilk_wf_on(n2, "pass", n3);

    csilk_wf_run(wf, nullptr, on_agent_complete);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(g_n1_count == 3);
    assert(g_done == 1);

    csilk_wf_free(wf);
    printf("test_workflow_agentic: PASS\n");
}

int
main()
{
    test_workflow_agentic();
    return 0;
}
