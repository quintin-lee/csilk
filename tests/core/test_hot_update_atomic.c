#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/server/hot_reload.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
dummy_v1_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "v1");
}

static void
dummy_v2_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "v2");
}

static void
test_hot_update_invalid_args(void)
{
    printf("Testing csilk_hot_update parameter validation...\n");

    csilk_router_t* r = csilk_router_new();
    csilk_server_t* server = csilk_server_new(r);
    assert(server != nullptr);

    csilk_hot_update_bundle_t bundle = {0};

    /* NULL server */
    assert(csilk_hot_update(nullptr, &bundle) == -1);

    /* NULL bundle */
    assert(csilk_hot_update(server, nullptr) == -1);

    /* Empty bundle (all members null) */
    assert(csilk_hot_update(server, &bundle) == -1);

    csilk_server_free(server);
    printf("  csilk_hot_update parameter validation passed!\n");
}

static void
test_hot_update_router_and_config(void)
{
    printf("Testing csilk_hot_update atomic router & config update...\n");

    /* Initial router v1 */
    csilk_router_t* r1 = csilk_router_new();
    csilk_handler_t h1 = dummy_v1_handler;
    csilk_router_add(r1, "GET", "/api/version", &h1, 1);

    csilk_server_t* server = csilk_server_new(r1);
    assert(server != nullptr);

    csilk_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.idle_timeout_ms = 5000;
    csilk_server_set_config(server, &cfg);

    /* Verify router v1 is active */
    csilk_rcu_token_t token;
    csilk_router_t*   active_r = csilk_server_router_acquire(server, &token);
    assert(active_r == r1);
    csilk_server_router_release(server, &token);

    /* Prepare hot update bundle */
    csilk_router_t* r2 = csilk_router_new();
    csilk_handler_t h2 = dummy_v2_handler;
    csilk_router_add(r2, "GET", "/api/version", &h2, 1);

    csilk_server_config_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));
    new_cfg.idle_timeout_ms = 15000;
    new_cfg.max_connections = 250;

    csilk_hot_update_bundle_t bundle = {
        .router = r2,
        .wasm_handle = nullptr,
        .tmp_wasm_path = nullptr,
        .config = &new_cfg,
    };

    int rc = csilk_hot_update(server, &bundle);
    assert(rc == 0);

    /* Verify router is now r2 */
    active_r = csilk_server_router_acquire(server, &token);
    assert(active_r == r2);
    csilk_server_router_release(server, &token);

    /* Verify server config was updated via csilk_server_get_config */
    csilk_server_config_t curr_cfg;
    assert(csilk_server_get_config(server, &curr_cfg) == 0);
    assert(curr_cfg.idle_timeout_ms == 15000);
    assert(curr_cfg.max_connections == 250);

    csilk_server_wait_grace_period(server);
    csilk_server_free(server);
    printf("  csilk_hot_update atomic router & config update passed!\n");
}

int
main(void)
{
    test_hot_update_invalid_args();
    test_hot_update_router_and_config();
    printf("All atomic hot update tests passed!\n");
    return 0;
}
