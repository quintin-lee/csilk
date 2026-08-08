#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/wasm.h"

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

    char  err_buf[128];
    char* output =
        csilk_wasm_plugin_exec(plugin, "run", "{\"infinite_loop\":true}", err_buf, sizeof(err_buf));
    assert(output == nullptr);
    assert(strstr(err_buf, "CSILK_WASM_TRAP_FUEL_EXHAUSTED") != nullptr);

    csilk_wasm_plugin_free(plugin);
    remove(dummy_wasm);
    printf("test_wasm_fuel_exhaustion_trap passed\n");
}

int
main(void)
{
    test_wasm_fuel_exhaustion_trap();
    printf("All test_wasm_fuel tests passed successfully!\n");
    return 0;
}
