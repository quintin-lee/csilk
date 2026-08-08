#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/wasm.h"

uint32_t csilk_host_get_input(void*       plugin,
                              uint32_t    memory_offset,
                              uint32_t    max_len,
                              const char* json_input);

static void
test_wasm_wasi_and_host_functions(void)
{
    const char* dummy_wasm = "test_wasi.wasm";
    FILE*       f = fopen(dummy_wasm, "wb");
    assert(f != nullptr);

    uint8_t header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    fwrite(header, 1, 8, f);
    fclose(f);

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load_file(dummy_wasm, nullptr);
    assert(plugin != nullptr);

    uint32_t nwritten = csilk_host_get_input(plugin, 0, 64, "{\"msg\":\"wasi_test\"}");
    assert(nwritten > 0);

    csilk_wasm_plugin_free(plugin);
    remove(dummy_wasm);
    printf("test_wasm_wasi_and_host_functions passed\n");
}

int
main(void)
{
    test_wasm_wasi_and_host_functions();
    printf("All test_wasm_wasi tests passed successfully!\n");
    return 0;
}
