/**
 * @file db_plugin.c
 * @brief csilk-db plugin entry point.
 *
 * Registers the csilk-db plugin with the core server when csilk_db.so is
 * loaded via csilk_core_load_plugin().  Database drivers (SQLite, MySQL,
 * PostgreSQL, Redis, MongoDB) self-register when csilk_db_init() is called
 * by the application after loading the plugin.
 *
 * @copyright MIT License
 */

#include "csilk/core/plugin.h"
#include "csilk/drivers/db.h"

static int
db_plugin_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
db_plugin_cleanup(void)
{
    /* No dynamic state to tear down. */
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
                   .name = "csilk-db",
                   .description = "Database drivers (SQLite, MySQL, PostgreSQL, Redis, MongoDB)",
                   },
        .init = db_plugin_init,
        .cleanup = db_plugin_cleanup,
    };
    return csilk_core_register_plugin(ctx->server, &plugin);
}
#endif
