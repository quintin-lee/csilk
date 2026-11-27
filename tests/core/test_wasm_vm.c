#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/plugin/wasm.h"

static void
test_wasm_vm_parsing_and_exec(void)
{
    /* Bad path / bad magic rejection */
    assert(csilk_wasm_plugin_load_file(nullptr, nullptr) == nullptr);
    assert(csilk_wasm_plugin_load_file("/nonexistent/csilk_missing.wasm", nullptr) == nullptr);

    const char* bad_wasm = "test_bad.wasm";
    FILE*       bf = fopen(bad_wasm, "wb");
    assert(bf != nullptr);
    uint8_t bad_bytes[8] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x00, 0x00, 0x00};
    fwrite(bad_bytes, 1, sizeof(bad_bytes), bf);
    fclose(bf);
    assert(csilk_wasm_plugin_load_file(bad_wasm, nullptr) == nullptr);
    remove(bad_wasm);

    /* Minimal valid WAT-compiled WASM module: (module (memory 1) (func (export "run"))) */
    static const uint8_t minimal_run_wasm[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00,
        0x00, 0x03, 0x02, 0x01, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x07, 0x01,
        0x03, 0x72, 0x75, 0x6e, 0x00, 0x00, 0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b};

    const char* dummy_wasm = "test_dummy.wasm";
    FILE*       f = fopen(dummy_wasm, "wb");
    assert(f != nullptr);
    fwrite(minimal_run_wasm, 1, sizeof(minimal_run_wasm), f);
    fclose(f);

    csilk_wasm_config_t config;
    config.max_memory_pages = 64;
    config.fuel_limit = 500000;

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load_file(dummy_wasm, &config);
    assert(plugin != nullptr);

    char  err_buf[128] = {0};
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
