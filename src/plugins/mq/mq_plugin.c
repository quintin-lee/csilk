/**
 * @file mq_plugin.c
 * @brief csilk-mq plugin entry point.
 *
 * Registers the csilk-mq plugin with the core server when csilk_mq.so is
 * loaded via csilk_core_load_plugin().  The message queue is lazily
 * initialized by the server core on first access.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

#ifdef CSILK_PLUGIN_BUILD

static int
mq_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
mq_plugin_cleanup(void)
{
    /* No dynamic state to tear down. */
}

CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION,
                   .plugin_version = 0x00010001,
                   .name = "csilk-mq",
                   .description = "In-process message queue with pub/sub, WAL, and Raft consensus",
                   },
        .init = mq_plugin_init,
        .cleanup = mq_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}

#endif
