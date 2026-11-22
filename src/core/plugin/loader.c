/**
 * @file loader.c
 * @brief Plugin loader — dlopen/dlsym-based runtime plugin & service management.
 *
 * Implements csilk_core_load_plugin, csilk_core_unload_plugin,
 * csilk_core_register_plugin, csilk_core_find_plugin, and the per-server
 * cross-plugin service locator (csilk_core_service_register /
 * csilk_core_service_get).
 *
 * Thread-safety: all registry mutations and lookups are serialized under
 * g_registry_mutex (lazily initialized via csilk_once), while dlopen/dlsym,
 * plugin entry/init/cleanup, and dlclose execute outside the lock so plugins
 * may call back into registration/service APIs without deadlocking.
 *
 * @copyright MIT License
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/plugin.h"
#include "csilk/core/service_locator.h"
#include "csilk/core/server/server.h"
#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "../internal/srv_impl.h"

enum {
    CSILK_MAX_PLUGINS = 32,
    CSILK_MAX_SERVICES = 64,
    CSILK_MAX_PLUGIN_PATH = 512,
};

typedef enum {
    PLUGIN_SLOT_EMPTY = 0,
    PLUGIN_SLOT_LOADING = 1,
    PLUGIN_SLOT_ACTIVE = 2,
} plugin_slot_state_t;

typedef struct {
    plugin_slot_state_t   state;
    csilk_server_t*       server;
    const csilk_plugin_t* plugin;
    void*                 handle;
} csilk_plugin_registry_entry_t;

typedef struct {
    int             active;
    csilk_server_t* server;
    char*           name;
    void*           service;
} csilk_service_registry_entry_t;

typedef union {
    void*                ptr;
    csilk_plugin_init_fn fn;
} plugin_dlsym_u;

static void                            plugin_registry_init(void);
static bool                            plugin_is_safe_path(const char* path);
static int                             plugin_find_empty_slot_locked(void);
static csilk_plugin_registry_entry_t*  plugin_find_by_name_locked(csilk_server_t* server,
                                                                  const char*     name);
static int                             service_find_empty_slot_locked(void);
static csilk_service_registry_entry_t* service_find_by_name_locked(csilk_server_t* server,
                                                                   const char*     name);

static csilk_once_t                   g_registry_once = CSILK_ONCE_INIT;
static csilk_mutex_t                  g_registry_mutex;
static csilk_plugin_registry_entry_t  g_plugin_registry[CSILK_MAX_PLUGINS];
static int                            g_plugin_count = 0;
static csilk_service_registry_entry_t g_service_registry[CSILK_MAX_SERVICES];
static int                            g_service_count = 0;

static void
plugin_registry_init(void)
{
    csilk_mutex_init(&g_registry_mutex);
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        g_plugin_registry[i].state = PLUGIN_SLOT_EMPTY;
        g_plugin_registry[i].server = NULL;
        g_plugin_registry[i].plugin = NULL;
        g_plugin_registry[i].handle = NULL;
    }
    for (int i = 0; i < CSILK_MAX_SERVICES; i++) {
        g_service_registry[i].active = 0;
        g_service_registry[i].server = NULL;
        g_service_registry[i].name = NULL;
        g_service_registry[i].service = NULL;
    }
    g_plugin_count = 0;
    g_service_count = 0;
}

static bool
plugin_is_safe_path(const char* path)
{
    if (!path || path[0] == '\0') {
        return false;
    }
    size_t len = strlen(path);
    if (len >= CSILK_MAX_PLUGIN_PATH) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }
    if (strstr(path, ".so") == NULL && strstr(path, ".dylib") == NULL) {
        return false;
    }
    return true;
}

static int
plugin_find_empty_slot_locked(void)
{
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].state == PLUGIN_SLOT_EMPTY) {
            return i;
        }
    }
    return -1;
}

static csilk_plugin_registry_entry_t*
plugin_find_by_name_locked(csilk_server_t* server, const char* name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < CSILK_MAX_PLUGINS; i++) {
        if (g_plugin_registry[i].state == PLUGIN_SLOT_ACTIVE &&
            g_plugin_registry[i].server == server && g_plugin_registry[i].plugin != NULL &&
            g_plugin_registry[i].plugin->info.name != NULL &&
            strcmp(g_plugin_registry[i].plugin->info.name, name) == 0) {
            return &g_plugin_registry[i];
        }
    }
    return NULL;
}

static int
service_find_empty_slot_locked(void)
{
    for (int i = 0; i < CSILK_MAX_SERVICES; i++) {
        if (!g_service_registry[i].active) {
            return i;
        }
    }
    return -1;
}

static csilk_service_registry_entry_t*
service_find_by_name_locked(csilk_server_t* server, const char* name)
{
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < CSILK_MAX_SERVICES; i++) {
        if (g_service_registry[i].active && g_service_registry[i].server == server &&
            g_service_registry[i].name != NULL && strcmp(g_service_registry[i].name, name) == 0) {
            return &g_service_registry[i];
        }
    }
    return NULL;
}

int
csilk_core_load_plugin(csilk_server_t* server, const char* path)
{
    if (!server || !plugin_is_safe_path(path)) {
        return -1;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        CSILK_LOG_E("Plugin: dlopen('%s') failed: %s", path, err ? err : "unknown");
        return -1;
    }

    plugin_dlsym_u sym;
    sym.ptr = dlsym(handle, "csilk_plugin_entry");
    if (!sym.fn) {
        const char* err = dlerror();
        CSILK_LOG_E("Plugin: dlsym(csilk_plugin_entry) failed for '%s': %s",
                    path,
                    err ? err : "symbol not found");
        dlclose(handle);
        return -1;
    }

    /* Reserve a LOADING slot before invoking entry(&ctx) so the plugin's call
     * to csilk_core_register_plugin() can bind into this slot synchronously. */
    csilk_mutex_lock(&g_registry_mutex);
    int slot = plugin_find_empty_slot_locked();
    if (slot < 0) {
        csilk_mutex_unlock(&g_registry_mutex);
        dlclose(handle);
        return -1;
    }
    g_plugin_registry[slot].state = PLUGIN_SLOT_LOADING;
    g_plugin_registry[slot].server = server;
    g_plugin_registry[slot].handle = handle;
    g_plugin_registry[slot].plugin = NULL;
    g_plugin_count++;
    csilk_mutex_unlock(&g_registry_mutex);

    csilk_rcu_token_t  rcu_tok = {0};
    csilk_router_t*    router = csilk_server_router_acquire(server, &rcu_tok);
    csilk_plugin_ctx_t ctx = {
        .server = server,
        .router = router,
        .config_path = NULL,
        .user_data = handle,
    };

    int entry_rc = sym.fn(&ctx);

    csilk_mutex_lock(&g_registry_mutex);
    const csilk_plugin_t* plug = g_plugin_registry[slot].plugin;
    plugin_slot_state_t   slot_state = g_plugin_registry[slot].state;
    csilk_mutex_unlock(&g_registry_mutex);

    int ok = (entry_rc == 0 && slot_state == PLUGIN_SLOT_ACTIVE && plug != NULL);
    if (ok && plug->init != NULL) {
        if (plug->init(&ctx) != 0) {
            ok = 0;
        }
    }

    csilk_server_router_release(server, &rcu_tok);

    if (!ok) {
        csilk_mutex_lock(&g_registry_mutex);
        g_plugin_registry[slot].state = PLUGIN_SLOT_EMPTY;
        g_plugin_registry[slot].server = NULL;
        g_plugin_registry[slot].plugin = NULL;
        g_plugin_registry[slot].handle = NULL;
        g_plugin_count--;
        csilk_mutex_unlock(&g_registry_mutex);
        dlclose(handle);
        return -1;
    }

    return 0;
}

int
csilk_core_unload_plugin(csilk_server_t* server, const char* name)
{
    if (!server || !name || name[0] == '\0') {
        return -1;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_mutex_lock(&g_registry_mutex);
    csilk_plugin_registry_entry_t* entry = plugin_find_by_name_locked(server, name);
    if (!entry) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }

    csilk_plugin_cleanup_fn cleanup = (entry->plugin != NULL) ? entry->plugin->cleanup : NULL;
    void*                   handle = entry->handle;

    entry->state = PLUGIN_SLOT_EMPTY;
    entry->server = NULL;
    entry->plugin = NULL;
    entry->handle = NULL;
    g_plugin_count--;
    csilk_mutex_unlock(&g_registry_mutex);

    if (cleanup != NULL) {
        cleanup();
    }
    if (handle != NULL) {
        dlclose(handle);
    }

    return 0;
}

int
csilk_core_register_plugin(csilk_server_t* server, const csilk_plugin_t* plugin)
{
    if (!server || !plugin || plugin->info.abi_version != CSILK_PLUGIN_ABI_VERSION ||
        !plugin->info.name || plugin->info.name[0] == '\0') {
        return -1;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_mutex_lock(&g_registry_mutex);
    if (plugin_find_by_name_locked(server, plugin->info.name) != NULL) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }

    /* Prefer the slot currently being loaded on behalf of this server. */
    for (int i = CSILK_MAX_PLUGINS - 1; i >= 0; i--) {
        if (g_plugin_registry[i].state == PLUGIN_SLOT_LOADING &&
            g_plugin_registry[i].server == server && g_plugin_registry[i].plugin == NULL) {
            g_plugin_registry[i].plugin = plugin;
            g_plugin_registry[i].state = PLUGIN_SLOT_ACTIVE;
            csilk_mutex_unlock(&g_registry_mutex);
            return 0;
        }
    }

    /* Fallback for static/direct registration without csilk_core_load_plugin(). */
    int slot = plugin_find_empty_slot_locked();
    if (slot < 0) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }
    g_plugin_registry[slot].state = PLUGIN_SLOT_ACTIVE;
    g_plugin_registry[slot].server = server;
    g_plugin_registry[slot].plugin = plugin;
    g_plugin_registry[slot].handle = NULL;
    g_plugin_count++;
    csilk_mutex_unlock(&g_registry_mutex);

    return 0;
}

const csilk_plugin_t*
csilk_core_find_plugin(csilk_server_t* server, const char* name)
{
    if (!server || !name || name[0] == '\0') {
        return NULL;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_mutex_lock(&g_registry_mutex);
    csilk_plugin_registry_entry_t* entry = plugin_find_by_name_locked(server, name);
    const csilk_plugin_t*          plugin = entry ? entry->plugin : NULL;
    csilk_mutex_unlock(&g_registry_mutex);
    return plugin;
}

int
csilk_core_service_register(csilk_server_t* server, const char* name, void* service)
{
    if (!server || !name || name[0] == '\0' || !service) {
        return -1;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_mutex_lock(&g_registry_mutex);
    if (service_find_by_name_locked(server, name) != NULL) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }

    int slot = service_find_empty_slot_locked();
    if (slot < 0) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }

    char* name_copy = strdup(name);
    if (!name_copy) {
        csilk_mutex_unlock(&g_registry_mutex);
        return -1;
    }

    g_service_registry[slot].active = 1;
    g_service_registry[slot].server = server;
    g_service_registry[slot].name = name_copy;
    g_service_registry[slot].service = service;
    g_service_count++;
    csilk_mutex_unlock(&g_registry_mutex);

    return 0;
}

void*
csilk_core_service_get(csilk_server_t* server, const char* name)
{
    if (!server || !name || name[0] == '\0') {
        return NULL;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_mutex_lock(&g_registry_mutex);
    csilk_service_registry_entry_t* entry = service_find_by_name_locked(server, name);
    void*                           svc = entry ? entry->service : NULL;
    csilk_mutex_unlock(&g_registry_mutex);
    return svc;
}

CSILK_INTERNAL void
_csilk_plugin_cleanup_server(csilk_server_t* server)
{
    if (!server) {
        return;
    }

    csilk_once(&g_registry_once, plugin_registry_init);

    csilk_plugin_cleanup_fn cleanups[CSILK_MAX_PLUGINS];
    void*                   handles[CSILK_MAX_PLUGINS];
    int                     unload_count = 0;

    csilk_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < CSILK_MAX_SERVICES; i++) {
        if (g_service_registry[i].active && g_service_registry[i].server == server) {
            free(g_service_registry[i].name);
            g_service_registry[i].name = NULL;
            g_service_registry[i].service = NULL;
            g_service_registry[i].server = NULL;
            g_service_registry[i].active = 0;
            g_service_count--;
        }
    }

    for (int i = CSILK_MAX_PLUGINS - 1; i >= 0; i--) {
        if (g_plugin_registry[i].state == PLUGIN_SLOT_ACTIVE &&
            g_plugin_registry[i].server == server) {
            cleanups[unload_count] =
                (g_plugin_registry[i].plugin != NULL) ? g_plugin_registry[i].plugin->cleanup : NULL;
            handles[unload_count] = g_plugin_registry[i].handle;
            unload_count++;

            g_plugin_registry[i].state = PLUGIN_SLOT_EMPTY;
            g_plugin_registry[i].server = NULL;
            g_plugin_registry[i].plugin = NULL;
            g_plugin_registry[i].handle = NULL;
            g_plugin_count--;
        }
    }
    csilk_mutex_unlock(&g_registry_mutex);

    for (int i = 0; i < unload_count; i++) {
        if (cleanups[i] != NULL) {
            cleanups[i]();
        }
        if (handles[i] != NULL) {
            dlclose(handles[i]);
        }
    }
}
