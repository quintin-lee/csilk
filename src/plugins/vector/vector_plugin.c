/**
 * @file vector_plugin.c
 * @brief csilk-vector plugin entry point.
 *
 * Registers the csilk-vector plugin with the core server.  Provides HNSW
 * vector index and SIMD distance kernels for similarity search.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

static int
vector_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
vector_plugin_cleanup(void)
{
}

#ifdef CSILK_PLUGIN_BUILD
CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION,
                   .plugin_version = 0x00010001,
                   .name = "csilk-vector",
                   .description = "HNSW SIMD vector index engine for similarity search",
                   },
        .init = vector_plugin_init,
        .cleanup = vector_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}
#endif
