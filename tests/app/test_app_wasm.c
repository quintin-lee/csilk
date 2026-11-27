#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "csilk/app/app.h"
#include "csilk/app/workflow.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/csilk.h"

int
main(void)
{
    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != NULL);

    /* Null safety checks */
    assert(csilk_app_use_wasm(NULL, NULL, NULL) == -1);
    assert(csilk_app_use_wasm(app, NULL, "on_request") == -1);

    static const uint8_t dummy_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(dummy_wasm, sizeof(dummy_wasm));
#ifdef HAS_WAMR
    assert(plugin != NULL);
    int rc = csilk_app_use_wasm(app, plugin, "on_request");
    assert(rc == 0);

    /* Test workflow wasm node with preloaded plugin */
    csilk_wf_t* wf = csilk_wf_new("test_wf_wasm");
    assert(wf != NULL);
    rc = csilk_wf_add_wasm_plugin_node(wf, "wasm_node_1", plugin, "on_request");
    assert(rc == 0);
    csilk_wf_free(wf);

    csilk_wasm_plugin_free(plugin);
#else
    printf("test_app_wasm (fail-closed stub check)\n");
#endif

    csilk_app_free(app);
    printf("test_app_wasm passed successfully\n");
    return 0;
}
