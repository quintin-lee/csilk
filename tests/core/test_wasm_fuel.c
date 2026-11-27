#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin/wasm.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/plugin/wasm_sdk.h"

static void
test_wasm_fuel_api(void)
{
    /* Null pointer safety check in all build variants */
    assert(csilk_wasm_plugin_set_fuel(nullptr, 5000) == -1);
    assert(csilk_wasm_plugin_get_fuel(nullptr) == 0);

    static const uint8_t dummy_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* p = csilk_wasm_plugin_load(dummy_wasm, sizeof(dummy_wasm));
    assert(p != nullptr);

    int rc = csilk_wasm_plugin_set_fuel(p, 5000);
    assert(rc == 0);
    assert(csilk_wasm_plugin_get_fuel(p) == 5000);

    /* Zero fuel returns error code on execute */
    rc = csilk_wasm_plugin_set_fuel(p, 0);
    assert(rc == 0);
    assert(csilk_wasm_plugin_get_fuel(p) == 0);
    int exec_rc = csilk_wasm_plugin_execute(p, nullptr, "on_request");
    assert(exec_rc == CSILK_WASM_ERR_OUT_OF_FUEL || exec_rc == -1);

    csilk_wasm_plugin_free(p);
    printf("test_wasm_fuel_api passed\n");
}

static void
test_wasm_fuel_exhaustion_trap(void)
{
    const char* dummy_wasm = "test_loop.wasm";
    FILE*       f = fopen(dummy_wasm, "wb");
    assert(f != nullptr);

    uint8_t header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    fwrite(header, 1, 8, f);
    fclose(f);

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load_file(dummy_wasm, nullptr);
    assert(plugin != nullptr);

    char  err_buf[128] = {0};
    char* output =
        csilk_wasm_plugin_exec(plugin, "run", "{\"infinite_loop\":true}", err_buf, sizeof(err_buf));
    assert(output == nullptr);
    assert(err_buf[0] != '\0');

    csilk_wasm_plugin_free(plugin);
    remove(dummy_wasm);
    printf("test_wasm_fuel_exhaustion_trap passed\n");
}

int
main(void)
{
    test_wasm_fuel_api();
    test_wasm_fuel_exhaustion_trap();
    printf("All test_wasm_fuel tests passed successfully!\n");
    return 0;
}
