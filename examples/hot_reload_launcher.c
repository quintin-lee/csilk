/**
 * @file hot_reload_launcher.c
 * @brief Runner for hot-reloadable csilk applications.
 */

#include "csilk/csilk.h"
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_so> [port]\n", argv[0]);
        return 1;
    }

    const char* so_path = argv[1];
    int         port = (argc > 2) ? atoi(argv[2]) : 8080;

    /* Create a server without a router initially (it will be swapped in) */
    csilk_server_t* server = csilk_server_new(nullptr);
    if (!server) {
        return 1;
    }

    /* Start hot-reload mechanism. This will load the SO and set the initial router. */
    if (csilk_dev_hot_reload_start(server, so_path, "csilk_app_init") != 0) {
        fprintf(stderr, "Failed to initialize hot-reload for %s\n", so_path);
        return 1;
    }

    printf("Dev server starting on port %d with hot-reload enabled...\n", port);
    csilk_server_run(server, port);

    csilk_server_free(server);
    return 0;
}
