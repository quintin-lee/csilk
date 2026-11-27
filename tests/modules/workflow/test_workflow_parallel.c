#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"

static int g_n2a_done = 0;
static int g_n2b_done = 0;
static int g_n3_done = 0;

csilk_data_t*
start_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    printf("Start Node\n");
    return csilk_wf_data_new(ctx, "init", nullptr);
}

csilk_data_t*
n2a_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    printf("N2a: sleeping...\n");
    usleep(100000); // 100ms
    g_n2a_done = 1;
    printf("N2a: done\n");
    return nullptr;
}

csilk_data_t*
n2b_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    printf("N2b: done\n");
    g_n2b_done = 1;
    return nullptr;
}

csilk_data_t*
n3_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)ctx;
    (void)input;
    (void)user_data;
    printf("N3 (Join) executing\n");
    assert(g_n2a_done == 1);
    assert(g_n2b_done == 1);
    g_n3_done = 1;
    return nullptr;
}

void
test_workflow_parallel()
{
    printf("Testing parallel workflow join...\n");
    g_n2a_done = 0;
    g_n2b_done = 0;
    g_n3_done = 0;

    csilk_wf_t* wf = csilk_wf_new("parallel_wf");

    csilk_wf_node_t* n_start = csilk_wf_add(wf, "start", start_handler, nullptr);
    csilk_wf_node_t* n_2a = csilk_wf_add(wf, "n2a", n2a_handler, nullptr);
    csilk_wf_node_t* n_2b = csilk_wf_add(wf, "n2b", n2b_handler, nullptr);
    csilk_wf_node_t* n_3 = csilk_wf_add(wf, "n3", n3_handler, nullptr);

    csilk_wf_bind(n_start, n_2a);
    csilk_wf_bind(n_start, n_2b);
    csilk_wf_bind(n_2a, n_3);
    csilk_wf_bind(n_2b, n_3);

    csilk_wf_run(wf, nullptr, nullptr);

    csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

    assert(g_n3_done == 1);

    csilk_wf_free(wf);
    printf("test_workflow_parallel: PASS\n");
}

int
main()
{
    test_workflow_parallel();
    return 0;
}
