/**
 * @file wasm_plugin_entry.c
 * @brief csilk-wasm plugin entry point.
 *
 * Registers the csilk-wasm plugin (WASM VM & WASI sandbox) with the core server.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

#ifdef CSILK_PLUGIN_BUILD

static int
wasm_plugin_mod_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
wasm_plugin_mod_cleanup(void)
{
}

CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION,
                   .plugin_version = 0x00010001,
                   .name = "csilk-wasm",
                   .description = "WebAssembly VM and WASI sandbox plugin runtime",
                   },
        .init = wasm_plugin_mod_init,
        .cleanup = wasm_plugin_mod_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}

#endif
