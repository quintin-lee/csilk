/**
 * @file protocol_plugin.c
 * @brief csilk-protocol plugin entry point.
 *
 * Registers the csilk-protocol plugin (WebSocket, OpenAPI, Swagger, MCP, gRPC)
 * with the core server.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

#ifdef CSILK_PLUGIN_BUILD

static int
protocol_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
protocol_plugin_cleanup(void)
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
                   .name = "csilk-protocol",
                   .description = "Protocol handlers (WebSocket, OpenAPI, Swagger, MCP, gRPC)",
                   },
        .init = protocol_plugin_init,
        .cleanup = protocol_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}

#endif
