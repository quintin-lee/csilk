#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/plugin/wasm.h"

static void
test_wasm_vm_parsing_and_exec(void)
{
    const char* dummy_wasm = "test_dummy.wasm";
    FILE*       f = fopen(dummy_wasm, "wb");
    assert(f != nullptr);

    /* Magic 0x6d736100 (\0asm) and Version 0x01000000 (1.0) */
    uint8_t header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    fwrite(header, 1, 8, f);
    fclose(f);

    csilk_wasm_config_t config;
    config.max_memory_pages = 64;
    config.fuel_limit = 500000;

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load_file(dummy_wasm, &config);
    assert(plugin != nullptr);

    char  err_buf[128];
    char* output =
        csilk_wasm_plugin_exec(plugin, "run", "{\"key\":\"val\"}", err_buf, sizeof(err_buf));
    assert(output != nullptr);
    assert(strstr(output, "wasm_execution_success") != nullptr);
    free(output);

    csilk_wasm_plugin_free(plugin);
    remove(dummy_wasm);
    printf("test_wasm_vm_parsing_and_exec passed\n");
}

int
main(void)
{
    test_wasm_vm_parsing_and_exec();
    printf("All test_wasm_vm tests passed successfully!\n");
    return 0;
}
