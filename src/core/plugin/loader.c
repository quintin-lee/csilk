/**
 * @file loader.c
 * @brief Plugin loader — dlopen/dlsym-based runtime plugin management.
 *
 * Implements csilk_core_load_plugin, csilk_core_unload_plugin,
 * csilk_core_register_plugin, and csilk_core_find_plugin.  Loaded plugins
 * are tracked in a fixed-size registry so they can be unloaded cleanly
 * during server shutdown.
 *
 * @copyright MIT License
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/plugin.h"
#include "csilk/core/server/server.h"

/** @brief Maximum number of concurrently loaded plugins. */
enum { CSILK_MAX_PLUGINS = 32 };

/** @brief Registry entry for a loaded plugin. */
typedef struct {
    const csilk_plugin_t* plugin; /**< Plugin metadata + entry points. */
    void*                 handle; /**< dlopen handle for dlclose on unload. */
    int                   slot;   /**< Registry slot index (-1 = empty). */
} csilk_plugin_registry_entry_t;

/** @brief Global plugin registry (one per process). */
static csilk_plugin_registry_entry_t g_plugin_registry[CSILK_MAX_PLUGINS];
static int                           g_plugin_count = 0;

/** @brief Find an empty slot in the registry.
 * @return Slot index, or -1 if full. */
static int
plugin_find_empty_slot(void)
{
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].slot < 0) {
            return i;
        }
    }
    return -1;
}

/** @brief Find a plugin by name.
 * @param server Unused in this implementation (kept for API consistency).
 * @param name   Plugin name to look up.
 * @return Pointer to registry entry, or NULL if not found. */
static csilk_plugin_registry_entry_t*
plugin_find_by_name(csilk_server_t* server, const char* name)
{
    (void)server;
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].slot >= 0 && g_plugin_registry[i].plugin != NULL &&
            g_plugin_registry[i].plugin->info.name != NULL &&
            strcmp(g_plugin_registry[i].plugin->info.name, name) == 0) {
            return &g_plugin_registry[i];
        }
    }
    return NULL;
}

/** @brief Initialize all registry slots to empty. */
static void
plugin_registry_init(void)
{
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        g_plugin_registry[i].slot = -1;
        g_plugin_registry[i].plugin = NULL;
        g_plugin_registry[i].handle = NULL;
    }
    g_plugin_count = 0;
}

/* Ensure the registry is initialized before first use. */
static int g_registry_initialized = 0;

int
csilk_core_load_plugin(csilk_server_t* server, const char* path)
{
    if (!server || !path) {
        return -1;
    }

    if (!g_registry_initialized) {
        plugin_registry_init();
        g_registry_initialized = 1;
    }

    if (g_plugin_count >= CSILK_MAX_PLUGINS) {
        return -1;
    }

    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        return -1;
    }

    csilk_plugin_init_fn entry = (csilk_plugin_init_fn)dlsym(handle, "csilk_plugin_entry");
    if (!entry) {
        dlclose(handle);
        return -1;
    }

    csilk_plugin_ctx_t ctx = {
        .server = server,
        .router = NULL, /**< Plugins acquire the router via csilk_server_router_acquire(). */
        .config_path = NULL,
        .user_data = handle,
    };

    if (entry(&ctx) != 0) {
        dlclose(handle);
        return -1;
    }

    int slot = plugin_find_empty_slot();
    if (slot < 0) {
        dlclose(handle);
        return -1;
    }

    g_plugin_registry[slot].slot = slot;
    g_plugin_registry[slot].handle = handle;
    g_plugin_registry[slot].plugin = NULL; /* set by register_plugin */
    g_plugin_count++;

    return 0;
}

int
csilk_core_unload_plugin(csilk_server_t* server, const char* name)
{
    (void)server;

    if (!g_registry_initialized || !name) {
        return -1;
    }

    csilk_plugin_registry_entry_t* entry = plugin_find_by_name(server, name);
    if (!entry) {
        return -1;
    }

    if (entry->plugin != NULL && entry->plugin->cleanup != NULL) {
        entry->plugin->cleanup();
    }

    if (entry->handle != NULL) {
        dlclose(entry->handle);
    }

    entry->slot = -1;
    entry->plugin = NULL;
    entry->handle = NULL;
    g_plugin_count--;

    return 0;
}

int
csilk_core_register_plugin(csilk_server_t* server, const csilk_plugin_t* plugin)
{
    (void)server;

    if (!g_registry_initialized || !plugin) {
        return -1;
    }

    /* Reject duplicate names. */
    if (plugin_find_by_name(server, plugin->info.name) != NULL) {
        return -1;
    }

    /* Find the most recently loaded slot without a plugin assigned. */
    for (int i = CSILK_MAX_PLUGINS - 1; i >= 0; i--) {
        if (g_plugin_registry[i].slot >= 0 && g_plugin_registry[i].plugin == NULL) {
            g_plugin_registry[i].plugin = plugin;
            return 0;
        }
    }

    return -1;
}

const csilk_plugin_t*
csilk_core_find_plugin(csilk_server_t* server, const char* name)
{
    csilk_plugin_registry_entry_t* entry = plugin_find_by_name(server, name);
    if (!entry) {
        return NULL;
    }
    return entry->plugin;
}
