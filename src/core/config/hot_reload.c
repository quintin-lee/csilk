/**
 * @file hot_reload.c
 * @brief Implementation of the Hot-Reload (Live Reload) mechanism.
 * @copyright MIT License
 */

#include "csilk/csilk.h"
#include "csilk/core/hot_reload.h"
#include "csilk/core/internal.h"
#include "../srv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef csilk_router_t* (*csilk_app_init_t)(void);

/** @brief Internal context for the hot-reload subsystem.
 *
 * Tracks the shared library handle, the watched file path, and the I/O
 * handles for filesystem events and debounce timer.  Created by
 * csilk_dev_hot_reload_start() and lives for the server lifetime. */
typedef struct {
    csilk_server_t*     server;         /**< Owning server (router is swapped on reload). */
    char*               lib_path;       /**< strdup'd path to the shared library to watch. */
    char*               init_sym;       /**< strdup'd name of the factory symbol. */
    void*               dl_handle;      /**< Current loaded library handle (nullptr on start). */
    csilk_io_fs_event_t fs_event;       /**< I/O filesystem watcher (libuv or io_uring). */
    csilk_io_timer_t    debounce_timer; /**< Debounce timer (100 ms). */
} hot_reload_ctx_t;

/** @brief Load a new shared library and atomically swap the server's router.
 *
 * On Linux/macOS: dlclose() old library, dlopen() new one, dlsym() the
 * factory function, call it to get a new router, then set it via
 * csilk_server_set_router().  The old router is freed automatically.
 * On Windows: equivalent via LoadLibrary/FreeLibrary/GetProcAddress.
 *
 * RTLD_NOW | RTLD_LOCAL ensures symbols are resolved immediately and the
 * library can be re-loaded on the next change event.
 *
 * @return 0 on success, -1 if any step fails (with an explanatory message
 *         printed to stderr). */
static int
load_and_swap_router(hot_reload_ctx_t* ctx)
{
#ifdef _WIN32
    if (ctx->dl_handle) {
        FreeLibrary((HMODULE)ctx->dl_handle);
        ctx->dl_handle = nullptr;
    }
    ctx->dl_handle = LoadLibraryA(ctx->lib_path);
    if (!ctx->dl_handle) {
        CSILK_LOG_E("[Hot-Reload] Failed to load library: %s", ctx->lib_path);
        return -1;
    }
    csilk_app_init_t init_fn =
        (csilk_app_init_t)GetProcAddress((HMODULE)ctx->dl_handle, ctx->init_sym);
    if (!init_fn) {
        CSILK_LOG_E("[Hot-Reload] Failed to find symbol: %s", ctx->init_sym);
        FreeLibrary((HMODULE)ctx->dl_handle);
        ctx->dl_handle = nullptr;
        return -1;
    }
#else
    if (ctx->dl_handle) {
        dlclose(ctx->dl_handle);
        ctx->dl_handle = nullptr;
    }
    /* Use RTLD_NOW to resolve all symbols immediately, and RTLD_LOCAL so we can reload it. */
    ctx->dl_handle = dlopen(ctx->lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!ctx->dl_handle) {
        CSILK_LOG_E("[Hot-Reload] Failed to load library: %s", dlerror());
        return -1;
    }

    /* Clear any old error */
    dlerror();

    csilk_app_init_t init_fn = (csilk_app_init_t)dlsym(ctx->dl_handle, ctx->init_sym);
    const char*      dlsym_error = dlerror();
    if (dlsym_error) {
        CSILK_LOG_E("[Hot-Reload] Failed to find symbol: %s", dlsym_error);
        goto fail;
    }
#endif

    csilk_router_t* new_router = init_fn();
    if (!new_router) {
        CSILK_LOG_E("[Hot-Reload] Initialization function returned nullptr");
        goto fail;
    }

    csilk_server_set_router(ctx->server, new_router);
    CSILK_LOG_I("[Hot-Reload] Successfully loaded and hot-swapped router from %s", ctx->lib_path);
    return 0;

fail:
#ifndef _WIN32
    dlclose(ctx->dl_handle);
#else
    FreeLibrary((HMODULE)ctx->dl_handle);
#endif
    ctx->dl_handle = nullptr;
    return -1;
}

/** @brief Debounce timer callback — triggers the actual reload after a quiet
 *  period.  The 100 ms window coalesces multiple rapid file-change events
 *  (common during save or compilation tooling) into a single reload. */
static void
on_debounce_timer(csilk_io_timer_t* handle)
{
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
    CSILK_LOG_I("[Hot-Reload] File change detected. Reloading %s...", ctx->lib_path);
    load_and_swap_router(ctx);
}

/** @brief Watch the file change event from the filesystem watcher.
 *
 * Called by the I/O backend whenever the shared library file is created, modified, or
 * renamed.  Restarts the debounce timer rather than reloading immediately,
 * so that rapid successive events (e.g. editor atomic-save writes a temp
 * file then renames) are collapsed into a single reload. */
static void
on_file_change(csilk_io_fs_event_t* handle, const char* filename, int events, int status)
{
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
    (void)filename;
    (void)events;
    (void)status;

    /* Debounce: restart a 100ms timer to wait for compilation/file writing to finish */
    csilk_io_timer_start(&ctx->debounce_timer, on_debounce_timer, 100, 0);
}

int
csilk_dev_hot_reload_start(csilk_server_t* server, const char* lib_path, const char* init_sym)
{
    if (!server || !lib_path || !init_sym) {
        return -1;
    }

    hot_reload_ctx_t* ctx = calloc(1, sizeof(hot_reload_ctx_t));
    if (!ctx) {
        return -1;
    }

    ctx->server = server;
    ctx->lib_path = strdup(lib_path);
    ctx->init_sym = strdup(init_sym);
    if (!ctx->lib_path || !ctx->init_sym) {
        free(ctx->lib_path);
        free(ctx->init_sym);
        free(ctx);
        return -1;
    }
    ctx->fs_event.data = ctx;
    ctx->debounce_timer.data = ctx;

    if (load_and_swap_router(ctx) != 0) {
        free(ctx->lib_path);
        free(ctx->init_sym);
        free(ctx);
        return -1;
    }

    csilk_io_loop_t* loop = server->loop;
    csilk_io_timer_init(loop, &ctx->debounce_timer);
    csilk_io_fs_event_init(loop, &ctx->fs_event);
    csilk_io_fs_event_start(&ctx->fs_event, on_file_change, ctx->lib_path, 0);

    CSILK_LOG_I("[Hot-Reload] Watching %s for changes...", ctx->lib_path);
    return 0;
}
