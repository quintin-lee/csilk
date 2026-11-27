/**
 * @file fuzz_wasm.c
 * @brief libFuzzer harness for WASM plugin byte-loading and WAMR validation.
 *
 * Feeds arbitrary byte buffers into csilk_wasm_plugin_load() and
 * csilk_wasm_plugin_execute() to catch crashes, UAF, and invalid section
 * parsing in the WAMR path.
 *
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/core/plugin/wasm.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(data, size);
    if (plugin) {
        /* Try executing a few function names to exercise the exec path */
        csilk_wasm_plugin_execute(plugin, NULL, "on_request");
        csilk_wasm_plugin_execute(plugin, NULL, "infinite_loop");
        csilk_wasm_plugin_free(plugin);
    }

    /* Also test the exec API directly with a valid header + garbage body */
    if (size >= 8) {
        csilk_wasm_plugin_t* p2 = csilk_wasm_plugin_load(data, size);
        if (p2) {
            char  err[256] = {0};
            char* res = csilk_wasm_plugin_exec(p2, "on_request", "null", err, sizeof(err));
            free(res); /* may be NULL on error */
            csilk_wasm_plugin_free(p2);
        }
    }

    return 0;
}
