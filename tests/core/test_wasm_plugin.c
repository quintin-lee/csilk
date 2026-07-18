#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/wasm_plugin.h"

static void
test_wasm_plugin_load_invalid()
{
    printf("Testing WASM plugin invalid magic header validation...\n");

    const uint8_t        invalid_bytes[8] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(invalid_bytes, sizeof(invalid_bytes));
    assert(plugin == NULL);

    assert(csilk_wasm_plugin_load(NULL, 10) == NULL);

    printf("test_wasm_plugin_load_invalid: PASS\n");
}

static void
test_wasm_plugin_load_valid()
{
    printf("Testing WASM plugin valid header load & execution...\n");

    /* Valid WASM header: \0asm followed by version 1 */
    const uint8_t valid_wasm[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(valid_wasm, sizeof(valid_wasm));
    assert(plugin != NULL);
    assert(csilk_wasm_plugin_is_valid(plugin) == 1);

    int res = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
    assert(res == 0);

    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_plugin_load_valid: PASS\n");
}

int
main()
{
    test_wasm_plugin_load_invalid();
    test_wasm_plugin_load_valid();
    printf("All WASM Plugin tests passed successfully!\n");
    return 0;
}
