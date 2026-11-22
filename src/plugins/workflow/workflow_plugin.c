/**
 * @file workflow_plugin.c
 * @brief csilk-workflow plugin entry point.
 *
 * Registers the csilk-workflow plugin with the core server.  The workflow
 * engine discovers AI and MQ services via the service locator at init time,
 * avoiding compile-time dependencies on those plugins.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"
#include "csilk/core/service_locator.h"

static int
workflow_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    /* Discover optional services; workflow can run without them but
     * degrades gracefully if AI or MQ are unavailable. */
    (void)csilk_core_service_get(ctx->server, "ai_engine");
    (void)csilk_core_service_get(ctx->server, "mq_bus");
    (void)ctx;
    return 0;
}

static void
workflow_plugin_cleanup(void)
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
                   .name = "csilk-workflow",
                   .description = "AI agent workflow engine with DAG scheduler and ReAct loop",
                   },
        .init = workflow_plugin_init,
        .cleanup = workflow_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}
#endif
