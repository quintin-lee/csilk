#pragma once
/**
 * @file plugin.h
 * @brief Plugin ABI interface for csilk modular extension.
 *
 * Defines the stable C ABI that every optional csilk plugin (ai, mq, db,
 * workflow, vector, protocol, wasm) must implement to be loaded
 * at runtime via dlopen.  The core library exposes csilk_core_load_plugin()
 * and csilk_core_register_plugin() so plugins can register middleware,
 * routes, drivers, and services without the core library linking against
 * plugin implementation sources.
 *
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/types.h"
#include "csilk/core/server/server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Plugin ABI version.  Every plugin must report this value in
 *  csilk_plugin_info_t::abi_version so the core can reject incompatible
 *  plugins at load time. */
#define CSILK_PLUGIN_ABI_VERSION 1

/** @brief Plugin metadata reported to the core at initialization. */
typedef struct {
    uint32_t    abi_version;    /**< Must equal CSILK_PLUGIN_ABI_VERSION. */
    uint32_t    plugin_version; /**< Plugin-specific version (semver packed). */
    const char* name;           /**< Human-readable plugin name (e.g. "csilk-ai"). */
    const char* description;    /**< Human-readable plugin description. */
} csilk_plugin_info_t;

/** @brief Initialization context passed to every plugin entry point.
 *
 * The core library fills this structure before invoking
 * csilk_plugin_entry.  Pointers remain valid for the lifetime of the
 * server. */
typedef struct {
    csilk_server_t* server;      /**< Server instance for registering middleware/routes. */
    csilk_router_t* router;      /**< Router instance for route registration. */
    const char*     config_path; /**< YAML config file path (may be NULL). */
    void*           user_data;   /**< Core-reserved; plugins must not touch. */
} csilk_plugin_ctx_t;

/** @brief Plugin entry point function signature.
 *
 * Every plugin shared library must export a symbol named
 * csilk_plugin_entry with this signature.  The core library locates it
 * via dlsym and invokes it once during plugin loading.
 *
 * @param ctx Initialization context provided by the core.
 * @return 0 on success, non-zero on failure. */
typedef int (*csilk_plugin_init_fn)(const csilk_plugin_ctx_t* ctx);

/** @brief Plugin cleanup function signature.
 * Invoked when the plugin is unloaded (dlclose). */
typedef void (*csilk_plugin_cleanup_fn)(void);

/** @brief Plugin export structure filled by csilk_plugin_entry. */
typedef struct {
    csilk_plugin_info_t     info;
    csilk_plugin_init_fn    init;
    csilk_plugin_cleanup_fn cleanup;
} csilk_plugin_t;

/** @brief Load a plugin shared library into a running server.
 *
 * Opens @p path with dlopen(RTLD_NOW | RTLD_LOCAL), locates the
 * csilk_plugin_entry symbol, and invokes it with a context bound to
 * @p server.  The plugin handle is retained internally so that
 * csilk_core_unload_plugin can dlclose it later.
 *
 * @param server Server instance to bind the plugin to.
 * @param path   Filesystem path to the plugin .so (or .dylib).
 * @return 0 on success, -1 on failure (see dlerror). */
int csilk_core_load_plugin(csilk_server_t* server, const char* path);

/** @brief Unload a previously loaded plugin by name.
 *
 * Looks up the plugin by @p name, invokes its cleanup callback if
 * present, then dlclose's the handle.  Safe to call on an already
 * unloaded or unknown name (returns -1).
 *
 * @param server Server instance the plugin was bound to.
 * @param name   Plugin name as reported in csilk_plugin_info_t::name.
 * @return 0 on success, -1 if the plugin is not found. */
int csilk_core_unload_plugin(csilk_server_t* server, const char* name);

/** @brief Register a loaded plugin with the core server.
 *
 * Invoked by a plugin's init function to announce itself.  Stores the
 * plugin's metadata in the server's plugin registry for inspection and
 * later unloading.
 *
 * @param server  Server instance.
 * @param plugin  Pointer to a static csilk_plugin_t filled by the plugin.
 * @return 0 on success, -1 if registration fails (e.g. duplicate name). */
int csilk_core_register_plugin(csilk_server_t* server, const csilk_plugin_t* plugin);

/** @brief Look up a registered plugin by name.
 *
 * @param server Server instance.
 * @param name   Plugin name.
 * @return Pointer to the registered csilk_plugin_t, or NULL if not found. */
const csilk_plugin_t* csilk_core_find_plugin(csilk_server_t* server, const char* name);

#ifdef __cplusplus
}
#endif
