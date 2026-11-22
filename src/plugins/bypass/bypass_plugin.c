/**
 * @file bypass_plugin.c
 * @brief csilk-bypass plugin entry point.
 *
 * Registers the csilk-bypass plugin (AF_XDP / DPDK kernel bypass I/O) with
 * the core server.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

#ifdef CSILK_PLUGIN_BUILD

static int
bypass_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
bypass_plugin_cleanup(void)
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
                   .name = "csilk-bypass",
                   .description = "Kernel-bypass networking drivers (AF_XDP, DPDK)",
                   },
        .init = bypass_plugin_init,
        .cleanup = bypass_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}

#endif
