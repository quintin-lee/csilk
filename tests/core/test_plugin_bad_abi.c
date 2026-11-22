/**
 * @file test_plugin_bad_abi.c
 * @brief Mock shared plugin reporting an incompatible ABI version.
 */

#include "csilk/core/plugin.h"

static int
bad_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
bad_cleanup(void)
{
}

CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION + 99U,
                   .plugin_version = 0x00010000,
                   .name = "test-bad-abi-plugin",
                   .description = "Incompatible ABI plugin for test_plugin",
                   },
        .init = bad_init,
        .cleanup = bad_cleanup,
    };
    if (!ctx || !ctx->server) {
        return -1;
    }
    return csilk_core_register_plugin(ctx->server, &plugin);
}
