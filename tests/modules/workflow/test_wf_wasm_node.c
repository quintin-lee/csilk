#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/workflow/dsl.h"
#include "csilk/wasm/wasm.h"

static void
test_workflow_wasm_node_execution(void)
{
    const char* dummy_wasm = "test_wf_wasm.wasm";
    FILE*       f = fopen(dummy_wasm, "wb");
    assert(f != NULL);

    uint8_t header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    fwrite(header, 1, 8, f);
    fclose(f);

    csilk_wf_t* wf = csilk_wf_new("wasm_pipeline");
    assert(wf != NULL);

#ifdef CSILK_HAS_WASM
    int res = csilk_wf_add_wasm_node(wf, "wasm_step_1", dummy_wasm);
    assert(res == 0);
#else
    errno = 0;
    int res = csilk_wf_add_wasm_node(wf, "wasm_step_1", dummy_wasm);
    assert(res == -1);
    assert(errno == ENOTSUP);

    errno = 0;
    res = csilk_wf_add_wasm_plugin_node(wf, "wasm_step_2", NULL, "run");
    assert(res == -1);
    assert(errno == ENOTSUP);
#endif

    csilk_wf_free(wf);
    remove(dummy_wasm);
    printf("test_workflow_wasm_node_execution passed\n");
}

int
main(void)
{
    test_workflow_wasm_node_execution();
    printf("All test_wf_wasm_node tests passed successfully!\n");
    return 0;
}
