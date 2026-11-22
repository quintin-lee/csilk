/**
 * @file test_plugin_mod.c
 * @brief Mock shared plugin used by test_plugin to exercise real dlopen loading.
 */

#include <stdatomic.h>

#include "csilk/core/plugin.h"
#include "csilk/core/service_locator.h"

typedef struct {
    atomic_int init_calls;
    atomic_int cleanup_calls;
} test_plugin_state_t;

static test_plugin_state_t g_test_plugin_state = {0};

static int
mod_init(const csilk_plugin_ctx_t* ctx)
{
    atomic_fetch_add(&g_test_plugin_state.init_calls, 1);
    if (!ctx || !ctx->server) {
        return -1;
    }
    return csilk_core_service_register(ctx->server, "test_mod_service", &g_test_plugin_state);
}

static void
mod_cleanup(void)
{
    atomic_fetch_add(&g_test_plugin_state.cleanup_calls, 1);
}

CSILK_INTERNAL int
csilk_plugin_entry(const csilk_plugin_ctx_t* ctx)
{
    static const csilk_plugin_t plugin = {
        .info =
            {
                   .abi_version = CSILK_PLUGIN_ABI_VERSION,
                   .plugin_version = 0x00010000,
                   .name = "test-mod-plugin",
                   .description = "Mock dynamic plugin for test_plugin",
                   },
        .init = mod_init,
        .cleanup = mod_cleanup,
    };
    if (!ctx || !ctx->server) {
        return -1;
    }
    return csilk_core_register_plugin(ctx->server, &plugin);
}
