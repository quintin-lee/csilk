/**
 * @file hot_reload.c
 * @brief Implementation of the Hot-Reload (Live Reload) mechanism.
 * @copyright MIT License
 */

#include "csilk/hot_reload.h"
#include "csilk/core/internal.h"
#include "core/srv_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef csilk_router_t* (*csilk_app_init_t)(void);

typedef struct {
	csilk_server_t* server;
	char* lib_path;
	char* init_sym;
	void* dl_handle;
	uv_fs_event_t fs_event;
	uv_timer_t debounce_timer;
} hot_reload_ctx_t;

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
		fprintf(stderr, "[Hot-Reload] Failed to load library: %s\n", ctx->lib_path);
		return -1;
	}
	csilk_app_init_t init_fn =
	    (csilk_app_init_t)GetProcAddress((HMODULE)ctx->dl_handle, ctx->init_sym);
	if (!init_fn) {
		fprintf(stderr, "[Hot-Reload] Failed to find symbol: %s\n", ctx->init_sym);
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
		fprintf(stderr, "[Hot-Reload] Failed to load library: %s\n", dlerror());
		return -1;
	}

	/* Clear any old error */
	dlerror();

	csilk_app_init_t init_fn = (csilk_app_init_t)dlsym(ctx->dl_handle, ctx->init_sym);
	const char* dlsym_error = dlerror();
	if (dlsym_error) {
		fprintf(stderr, "[Hot-Reload] Failed to find symbol: %s\n", dlsym_error);
		return -1;
	}
#endif

	csilk_router_t* new_router = init_fn();
	if (!new_router) {
		fprintf(stderr, "[Hot-Reload] Initialization function returned nullptr\n");
		return -1;
	}

	csilk_server_set_router(ctx->server, new_router);
	printf("[Hot-Reload] Successfully loaded and hot-swapped router from %s\n", ctx->lib_path);
	return 0;
}

static void
on_debounce_timer(uv_timer_t* handle)
{
	hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
	printf("[Hot-Reload] File change detected. Reloading %s...\n", ctx->lib_path);
	load_and_swap_router(ctx);
}

static void
on_file_change(uv_fs_event_t* handle, const char* filename, int events, int status)
{
	hot_reload_ctx_t* ctx = (hot_reload_ctx_t*)handle->data;
	(void)filename;
	(void)events;
	(void)status;

	/* Debounce: restart a 100ms timer to wait for compilation/file writing to finish */
	uv_timer_start(&ctx->debounce_timer, on_debounce_timer, 100, 0);
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
	ctx->fs_event.data = ctx;
	ctx->debounce_timer.data = ctx;

	if (load_and_swap_router(ctx) != 0) {
		free(ctx->lib_path);
		free(ctx->init_sym);
		free(ctx);
		return -1;
	}

	uv_loop_t* loop = server->loop;
	uv_timer_init(loop, &ctx->debounce_timer);
	uv_fs_event_init(loop, &ctx->fs_event);
	uv_fs_event_start(&ctx->fs_event, on_file_change, ctx->lib_path, 0);

	printf("[Hot-Reload] Watching %s for changes...\n", ctx->lib_path);
	return 0;
}
