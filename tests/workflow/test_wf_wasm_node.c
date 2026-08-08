#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/core/wasm.h"

static void
test_workflow_wasm_node_execution(void)
{
    csilk_wf_t* wf = csilk_wf_new("wasm_pipeline");
    assert(wf != nullptr);

    int res = csilk_wf_add_wasm_node(wf, "wasm_step_1", "plugins/sample.wasm");
    assert(res == 0);

    csilk_wf_free(wf);
    printf("test_workflow_wasm_node_execution passed\n");
}

int
main(void)
{
    test_workflow_wasm_node_execution();
    printf("All test_wf_wasm_node tests passed successfully!\n");
    return 0;
}
