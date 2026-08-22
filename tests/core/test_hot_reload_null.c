/**
 * @file test_hot_reload_null.c
 * @brief Null-safety unit tests for hot-reload API.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>

#include "csilk/csilk.h"
#include "csilk/core/hot_reload.h"

static void
test_reload_start_null_server(void)
{
    printf("Testing csilk_dev_hot_reload_start with NULL server...\n");
    int rc = csilk_dev_hot_reload_start(NULL, "/tmp/lib.so", "init");
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_reload_start_null_lib_path(void)
{
    printf("Testing csilk_dev_hot_reload_start with NULL lib_path...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);
    int rc = csilk_dev_hot_reload_start(server, NULL, "init");
    assert(rc == -1);
    csilk_server_free(server);
    csilk_router_free(router);
    printf("  passed\n");
}

static void
test_reload_start_null_init_sym(void)
{
    printf("Testing csilk_dev_hot_reload_start with NULL init_sym...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);
    int rc = csilk_dev_hot_reload_start(server, "/tmp/lib.so", NULL);
    assert(rc == -1);
    csilk_server_free(server);
    csilk_router_free(router);
    printf("  passed\n");
}

static void
test_reload_trigger_null_server(void)
{
    printf("Testing csilk_dev_hot_reload_trigger with NULL server...\n");
    int rc = csilk_dev_hot_reload_trigger(NULL);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_reload_trigger_uninitialized(void)
{
    printf("Testing csilk_dev_hot_reload_trigger on uninitialized server...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);
    int rc = csilk_dev_hot_reload_trigger(server);
    assert(rc == -1);
    csilk_server_free(server);
    csilk_router_free(router);
    printf("  passed\n");
}

static void
test_reload_stop_null_server(void)
{
    printf("Testing csilk_dev_hot_reload_stop with NULL server...\n");
    csilk_dev_hot_reload_stop(NULL);
    printf("  passed\n");
}

static void
test_reload_stop_uninitialized(void)
{
    printf("Testing csilk_dev_hot_reload_stop on uninitialized server...\n");
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);
    csilk_dev_hot_reload_stop(server);
    csilk_server_free(server);
    csilk_router_free(router);
    printf("  passed\n");
}

int
main(void)
{
    test_reload_start_null_server();
    test_reload_start_null_lib_path();
    test_reload_start_null_init_sym();
    test_reload_trigger_null_server();
    test_reload_trigger_uninitialized();
    test_reload_stop_null_server();
    test_reload_stop_uninitialized();

    printf("All test_hot_reload_null tests passed successfully!\n");
    return 0;
}
