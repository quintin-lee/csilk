/**
 * @file hot_reload.c
 * @brief Implementation of the Safe RCU / EBR Hot-Reload (Live Reload) mechanism.
 * @copyright MIT License
 */

#include "csilk/core/hot_reload.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef csilk_router_t* (*csilk_app_init_t)(void);

/** @brief Internal context for the hot-reload subsystem.
 *
 * Tracks the shared library handle, the watched file path, and the I/O
 * handles for filesystem events and debounce timer. Created by
 * csilk_dev_hot_reload_start() and lives for the server lifetime. */
typedef struct {
    csilk_server_t*     server;         /**< Owning server (router is swapped on reload). */
    char*               lib_path;       /**< strdup'd path to the shared library to watch. */
    char*               init_sym;       /**< strdup'd name of the factory symbol. */
    void*               dl_handle;      /**< Current loaded library handle (NULL on start). */
    char*               tmp_path;       /**< Current loaded temp file path (NULL on start). */
    csilk_io_fs_event_t fs_event;       /**< I/O filesystem watcher (libuv or io_uring). */
    csilk_io_timer_t    debounce_timer; /**< Debounce timer (100 ms). */
    int                 is_watching;    /**< 1 if filesystem watcher is active. */
} hot_reload_ctx_t;

/**
 * @brief Copy a file from src to dst.
 */
static int
copy_file(const char* src, const char* dst)
{
    FILE* in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[8192];
    int  err = 0;
    while (!feof(in) && !ferror(in)) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                err = 1;
                break;
            }
        }
    }
    if (ferror(in)) {
        err = 1;
    }
    fclose(in);
    fclose(out);

    if (err) {
        return -1;
    }

#ifndef _WIN32
    chmod(dst, 0755);
#endif
    return 0;
}

/**
 * @brief Create a uniquely named copy of the shared library to bypass dlopen caching.
 */
static int
create_temp_lib_copy(const char* src_path, char* out_path, size_t max_len)
{
#ifndef _WIN32
    snprintf(out_path, max_len, "/tmp/csilk_reload_XXXXXX");
    int fd = mkstemp(out_path);
    if (fd < 0) {
        snprintf(
            out_path, max_len, "/tmp/csilk_reload_%lu_%d.so", (unsigned long)time(NULL), rand());
    } else {
        close(fd);
    }
#else
    char temp_dir[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    snprintf(out_path,
             max_len,
             "%scsilk_reload_%lu_%d.dll",
             temp_dir,
             (unsigned long)time(NULL),
             rand());
#endif
    if (copy_file(src_path, out_path) != 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Load a new shared library instance and atomically swap the server's router.
 *
 * Uses RCU / EBR for safe concurrent hot reload:
 * 1. Creates a distinct temp copy of the target .so to bypass dynamic linker caching.
 * 2. dlopen()s the new library with RTLD_NOW | RTLD_LOCAL.
 * 3. Finds and executes the factory symbol to instantiate new_router.
 * 4. Calls csilk_server_set_router_full() to publish new_router atomically.
 * 5. The old router, old dl_handle, and old temp file are queued into the EBR
 *    retired list and will be reclaimed ONLY after all in-flight requests that
 *    observed the previous generation have finished.
 *
 * @return 0 on success, -1 on failure.
 */
static int
load_and_swap_router(hot_reload_ctx_t* ctx)
{
    char tmp_path[512];
    if (create_temp_lib_copy(ctx->lib_path, tmp_path, sizeof(tmp_path)) != 0) {
        CSILK_LOG_E("[Hot-Reload] Failed to create temp copy of %s", ctx->lib_path);
        return -1;
    }

    void*            new_handle = NULL;
    csilk_app_init_t init_fn = NULL;

#ifdef _WIN32
    new_handle = LoadLibraryA(tmp_path);
    if (!new_handle) {
        CSILK_LOG_E("[Hot-Reload] Failed to load library: %s", tmp_path);
        unlink(tmp_path);
        return -1;
    }
    init_fn = (csilk_app_init_t)GetProcAddress((HMODULE)new_handle, ctx->init_sym);
    if (!init_fn) {
        CSILK_LOG_E("[Hot-Reload] Failed to find symbol: %s", ctx->init_sym);
        FreeLibrary((HMODULE)new_handle);
        unlink(tmp_path);
        return -1;
    }
#else
    new_handle = dlopen(tmp_path, RTLD_NOW | RTLD_LOCAL);
    if (!new_handle) {
        CSILK_LOG_E("[Hot-Reload] Failed to load library: %s (%s)", tmp_path, dlerror());
        unlink(tmp_path);
        return -1;
    }

    dlerror(); /* Clear any old error */
    init_fn = (csilk_app_init_t)dlsym(new_handle, ctx->init_sym);
    const char* dlsym_error = dlerror();
    if (dlsym_error || !init_fn) {
        CSILK_LOG_E("[Hot-Reload] Failed to find symbol '%s': %s",
                    ctx->init_sym,
                    dlsym_error ? dlsym_error : "NULL symbol");
        dlclose(new_handle);
        unlink(tmp_path);
        return -1;
    }
#endif

    csilk_router_t* new_router = init_fn();
    if (!new_router) {
        CSILK_LOG_E("[Hot-Reload] Initialization function returned NULL router");
#ifndef _WIN32
        dlclose(new_handle);
#else
        FreeLibrary((HMODULE)new_handle);
#endif
        unlink(tmp_path);
        return -1;
    }

    void* old_handle = ctx->dl_handle;
    char* old_tmp = ctx->tmp_path;

    ctx->dl_handle = new_handle;
    ctx->tmp_path = strdup(tmp_path);

    /* Atomically swap router and schedule old resources for EBR grace-period reclamation */
    csilk_server_set_router_full(ctx->server, new_router, old_handle, old_tmp);
    free(old_tmp);

    CSILK_LOG_I("[Hot-Reload] Successfully published new router from %s (temp: %s)",
                ctx->lib_path,
                tmp_path);
    return 0;
}

/** @brief Debounce timer callback — triggers the actual reload after a quiet period. */
static void
on_debounce_timer(csilk_io_timer_t* handle)
{
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
    CSILK_LOG_I("[Hot-Reload] File change detected. Reloading %s...", ctx->lib_path);
    load_and_swap_router(ctx);
}

/** @brief Watch the file change event from the filesystem watcher. */
static void
on_file_change(csilk_io_fs_event_t* handle, const char* filename, int events, int status)
{
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
    (void)filename;
    (void)events;
    (void)status;

    /* Debounce: restart a 100ms timer to wait for compilation/file writing to settle */
    csilk_io_timer_start(&ctx->debounce_timer, on_debounce_timer, 100, 0);
}

int
csilk_dev_hot_reload_start(csilk_server_t* server, const char* lib_path, const char* init_sym)
{
    if (!server || !lib_path || !init_sym) {
        return -1;
    }

    /* Stop any existing watcher */
    if (server->hot_reload_ctx) {
        csilk_dev_hot_reload_stop(server);
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

    server->hot_reload_ctx = ctx;

    csilk_io_loop_t* loop = server->loop;
    if (loop) {
        csilk_io_timer_init(loop, &ctx->debounce_timer);
        csilk_io_fs_event_init(loop, &ctx->fs_event);
        csilk_io_fs_event_start(&ctx->fs_event, on_file_change, ctx->lib_path, 0);
        ctx->is_watching = 1;
    }

    CSILK_LOG_I("[Hot-Reload] Watching %s for changes...", ctx->lib_path);
    return 0;
}

int
csilk_dev_hot_reload_trigger(csilk_server_t* server)
{
    if (!server || !server->hot_reload_ctx) {
        return -1;
    }
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)server->hot_reload_ctx;
    return load_and_swap_router(ctx);
}

void
csilk_dev_hot_reload_stop(csilk_server_t* server)
{
    if (!server || !server->hot_reload_ctx) {
        return;
    }
    hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)server->hot_reload_ctx;
    server->hot_reload_ctx = NULL;

    if (ctx->is_watching) {
        csilk_io_timer_stop(&ctx->debounce_timer);
        csilk_io_fs_event_stop(&ctx->fs_event);
        csilk_io_close((csilk_io_handle_t*)&ctx->debounce_timer, NULL);
        csilk_io_close((csilk_io_handle_t*)&ctx->fs_event, NULL);
        ctx->is_watching = 0;
    }

    /* Grab the active router produced by hot reload so it is reclaimed by EBR */
    csilk_router_t* active_router =
        atomic_exchange_explicit(&server->router, NULL, memory_order_acq_rel);

    /* Schedule the active handle, active router, and temp file for EBR cleanup */
    if (active_router || ctx->dl_handle || ctx->tmp_path) {
        csilk_retired_router_t* rec = calloc(1, sizeof(csilk_retired_router_t));
        if (rec) {
            rec->router = active_router;
            rec->dl_handle = ctx->dl_handle;
            rec->tmp_path = ctx->tmp_path;
            rec->retired_epoch =
                atomic_load_explicit(&server->reload_mgr.global_epoch, memory_order_acquire);
            atomic_init(&rec->retired_next, NULL);

            csilk_retired_router_t* old_head =
                atomic_load_explicit(&server->reload_mgr.retired_head, memory_order_relaxed);
            do {
                atomic_store_explicit(&rec->retired_next, old_head, memory_order_relaxed);
            } while (!atomic_compare_exchange_weak_explicit(&server->reload_mgr.retired_head,
                                                            &old_head,
                                                            rec,
                                                            memory_order_release,
                                                            memory_order_relaxed));

            atomic_fetch_add_explicit(&server->reload_mgr.retired_count, 1, memory_order_relaxed);
        }
    }

    free(ctx->lib_path);
    free(ctx->init_sym);
    free(ctx);
}
