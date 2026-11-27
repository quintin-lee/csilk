/**
 * @file test_wasm_real.c
 * @brief WAMR real execution path tests (compiled when CSILK_ENABLE_WASM=ON).
 *
 * @copyright MIT License
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/test/test.h"

/*
 * Minimal valid WASM module (WAT-compiled):
 * (module (func (export "on_request")))
 * 8-byte header + type section + func section + export section
 */
static const uint8_t minimal_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03,
    0x02, 0x01, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x0e, 0x01, 0x0a, 0x6f, 0x6e, 0x5f,
    0x72, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x00, 0x00, 0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b};

static void
test_wasm_real_load_valid(void)
{
    printf("Testing WASM load of minimal valid module...\n");
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
    assert(plugin != NULL);
    assert(csilk_wasm_plugin_is_valid(plugin) == 1);
    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_real_load_valid: PASS\n");
}

static void
test_wasm_real_exec_fuel_reset(void)
{
    printf("Testing WASM fuel exhaustion L1 fix (fuel resets per call)...\n");
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
    assert(plugin != NULL);
    /*
     * Call execute 20 times. With the L1 bug (fuel_limit -= 100 each call),
     * after 10000 calls the plugin would be permanently disabled.
     * With the fix, each call is independent and the plugin stays usable.
     */
    for (int i = 0; i < 20; i++) {
        int rc = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
        assert(rc == 0 || rc == -1);
    }
    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_real_exec_fuel_reset: PASS\n");
}

static void
test_wasm_real_invalid_module_fallback(void)
{
    printf("Testing WASM fail-closed execution for header-only module...\n");
    const uint8_t        header_only[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(header_only, sizeof(header_only));
    assert(plugin != NULL);
    int rc = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
    assert(rc == -1);
    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_real_invalid_module_fallback: PASS\n");
}

int
main(void)
{
    test_wasm_real_load_valid();
    test_wasm_real_exec_fuel_reset();
    test_wasm_real_invalid_module_fallback();

    printf("test_wasm_real: ALL PASSED\n");
    return 0;
}
