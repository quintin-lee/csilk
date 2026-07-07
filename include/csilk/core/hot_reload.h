/**
 * @file hot_reload.h
 * @brief Live-reload (Hot Reload) development mechanism for csilk.
 *
 * @copyright MIT License
 */

#ifndef CSILK_HOT_RELOAD_H
#define CSILK_HOT_RELOAD_H

#include "csilk/csilk.h"

/**
 * @brief Initialize and start watching a shared library for hot-reloading.
 *
 * In development mode, the user compiles their route handlers into a shared
 * library (.so / .dylib) which exposes an initialization function.
 * This function loads the library, invokes the init function to get a
 * csilk_router_t, and attaches it to the server.
 * It also starts a file watcher (via libuv / io_uring fs_event). When the shared library is
 * updated, it automatically reloads the library and hot-swaps the router
 * without dropping the listening socket.
 *
 * @param server     The server instance.
 * @param lib_path   Path to the dynamic shared library (e.g., "./my_app.so").
 * @param init_sym   Symbol name of the initialization function. The function
 *                   must match the signature: `csilk_router_t* (*)(void)`.
 * @return 0 on success, -1 on failure.
 */
int csilk_dev_hot_reload_start(csilk_server_t* server, const char* lib_path, const char* init_sym);

#endif /* CSILK_HOT_RELOAD_H */
