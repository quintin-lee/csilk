#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/wasm_plugin.h"
#include "csilk/test/test.h"

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

static void
test_wasm_host_api()
{
    printf("Testing WASM Host C-ABI context interaction...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);
    csilk_test_ctx_set_request(ctx, "GET", "/test");
    csilk_test_ctx_add_param(ctx, "user", "alice");

    int res = csilk_wasm_host_set_header(ctx, "X-WASM-Filter", "PASSED");
    assert(res == 0);
    const char* val = csilk_wasm_host_get_header(ctx, "X-WASM-Filter");
    assert(val != NULL);
    assert(strcmp(val, "PASSED") == 0);

    const char* param = csilk_wasm_host_get_param(ctx, "user");
    assert(param != NULL);
    assert(strcmp(param, "alice") == 0);

    res = csilk_wasm_host_set_status(ctx, 403);
    assert(res == 0);
    assert(csilk_get_status(ctx) == 403);

    csilk_test_ctx_free(ctx);
    printf("test_wasm_host_api: PASS\n");
}

int
main()
{
    test_wasm_plugin_load_invalid();
    test_wasm_plugin_load_valid();
    test_wasm_host_api();
    printf("All WASM Plugin tests passed successfully!\n");
    return 0;
}
