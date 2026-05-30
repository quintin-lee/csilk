/**
 * @file hot_reload_app.c
 * @brief Example of a hot-reloadable application module.
 *
 * Compile this with:
 * gcc -shared -fPIC -o libhot_app.so examples/hot_reload_app.c -Iinclude -Lbuild -lcsilk
 */

#include "csilk/csilk.h"
#include <stdio.h>

void
hello_handler(csilk_ctx_t* c)
{
	/* Change this message, recompile, and see it update live! */
	csilk_string(c, 200, "Hello from Hot-Reloadable Module! (v1)");
}

/* Entry point for the hot-reload mechanism */
csilk_router_t*
csilk_app_init(void)
{
	csilk_router_t* r = csilk_router_new();
	csilk_router_add(r, "GET", "/", (csilk_handler_t[]){hello_handler}, 1);
	return r;
}
