/**
 * @file ai_plugin.c
 * @brief csilk-ai plugin entry point.
 *
 * Registers the csilk-ai plugin with the core server when csilk_ai.so is
 * loaded via csilk_core_load_plugin().  AI driver registration
 * (OpenAI, Ollama, Anthropic, Llama) is handled by csilk_ai_init() in
 * ai.c, which is invoked by the application after loading the plugin.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"

static int
ai_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
ai_plugin_cleanup(void)
{
    /* Drivers are statically registered; nothing to tear down. */
}

#ifdef CSILK_PLUGIN_BUILD
#ifdef CSILK_PLUGIN_BUILD
CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION,
                   .plugin_version = 0x00010001,
                   .name = "csilk-ai",
                   .description = "LLM client drivers (OpenAI, Ollama, Anthropic, Llama)",
                   },
        .init = ai_plugin_init,
        .cleanup = ai_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}
#endif
#endif
