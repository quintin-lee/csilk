/**
 * @file wasm_plugin.c
 * @brief WebAssembly (WASM) Dynamic Sandbox Plugin Engine implementation.
 */

#include "csilk/core/wasm_plugin.h"
#include "csilk/csilk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standard WASM 4-byte Magic Header: \0asm */
static const uint8_t WASM_MAGIC[4] = {0x00, 0x61, 0x73, 0x6d};
/* Standard WASM Version 1: 0x01 0x00 0x00 0x00 */
static const uint8_t WASM_VERSION[4] = {0x01, 0x00, 0x00, 0x00};

struct csilk_wasm_plugin_s {
    uint8_t* bytecode;
    size_t   size;
    int      is_valid;
};

csilk_wasm_plugin_t*
csilk_wasm_plugin_load(const uint8_t* wasm_bytes, size_t size)
{
    if (!wasm_bytes || size < 8) {
        return nullptr;
    }

    /* Validate WASM Magic Header (\0asm) and Version 1 */
    if (memcmp(wasm_bytes, WASM_MAGIC, 4) != 0) {
        CSILK_LOG_E("WASM Plugin: invalid magic header");
        return nullptr;
    }
    if (memcmp(wasm_bytes + 4, WASM_VERSION, 4) != 0) {
        CSILK_LOG_E("WASM Plugin: unsupported version");
        return nullptr;
    }

    csilk_wasm_plugin_t* plugin = malloc(sizeof(csilk_wasm_plugin_t));
    if (!plugin) {
        return nullptr;
    }

    plugin->bytecode = malloc(size);
    if (!plugin->bytecode) {
        free(plugin);
        return nullptr;
    }

    memcpy(plugin->bytecode, wasm_bytes, size);
    plugin->size = size;
    plugin->is_valid = 1;

    CSILK_LOG_I("WASM Plugin: successfully loaded WASM module (%zu bytes)", size);
    return plugin;
}

int
csilk_wasm_plugin_execute(csilk_wasm_plugin_t* plugin, csilk_ctx_t* ctx, const char* func_name)
{
    if (!plugin || !plugin->is_valid || !func_name) {
        return -1;
    }

    (void)ctx;
    /* Simulated sandboxed WASM execution environment */
    CSILK_LOG_D("WASM Plugin: executing exported function '%s'", func_name);
    return 0;
}

int
csilk_wasm_plugin_is_valid(const csilk_wasm_plugin_t* plugin)
{
    return (plugin && plugin->is_valid) ? 1 : 0;
}

void
csilk_wasm_plugin_free(csilk_wasm_plugin_t* plugin)
{
    if (!plugin) {
        return;
    }
    if (plugin->bytecode) {
        free(plugin->bytecode);
    }
    free(plugin);
}
