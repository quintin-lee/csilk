/**
 * @file test_plugin.c
 * @brief Unit and concurrency tests for dynamic plugin loader, ABI checks,
 *        service locator, and CSILK_PLUGIN_PATHS auto-loading.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin.h"
#include "csilk/core/service_locator.h"
#if defined(CSILK_REAL_PLUGIN_WORKFLOW) || defined(CSILK_REAL_PLUGIN_PROTOCOL)
#include "csilk/app/workflow.h"
#include "csilk/core/plugin/wasm.h"
#include "csilk/core/secret.h"
#include "csilk/crypto/crypto_dispatch.h"
#include "csilk/drivers/ai.h"
#include "csilk/drivers/ai/tool_registry.h"
#include "csilk/drivers/vector.h"
#include "csilk/messaging/mq.h"
#include "csilk/messaging/raft.h"
#endif

typedef struct {
    atomic_int init_calls;
    atomic_int cleanup_calls;
} test_plugin_state_t;

static int
dummy_static_init(const csilk_plugin_ctx_t* ctx)
{
    (void)ctx;
    return 0;
}

static void
test_plugin_load_init_service_and_unload(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(router != NULL && server != NULL);

    /* Load real compiled shared plugin via dlopen */
    assert(csilk_core_load_plugin(server, CSILK_TEST_PLUGIN_PATH) == 0);

    const csilk_plugin_t* plug = csilk_core_find_plugin(server, "test-mod-plugin");
    assert(plug != NULL);
    assert(plug->info.abi_version == CSILK_PLUGIN_ABI_VERSION);
    assert(strcmp(plug->info.name, "test-mod-plugin") == 0);

    /* Verify plugin->init() was invoked and registered its service */
    test_plugin_state_t* state =
        (test_plugin_state_t*)csilk_core_service_get(server, "test_mod_service");
    assert(state != NULL);
    assert(atomic_load(&state->init_calls) == 1);
    assert(atomic_load(&state->cleanup_calls) == 0);

    /* Reject loading duplicate plugin on the same server */
    assert(csilk_core_load_plugin(server, CSILK_TEST_PLUGIN_PATH) == -1);

    /* Unload cleanly and verify unknown/already-unloaded fails */
    assert(csilk_core_unload_plugin(server, "test-mod-plugin") == 0);
    assert(csilk_core_find_plugin(server, "test-mod-plugin") == NULL);
    assert(csilk_core_unload_plugin(server, "test-mod-plugin") == -1);

    csilk_server_free(server);
    csilk_router_free(router);
}

static void
test_plugin_security_and_validation(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

    /* Null/invalid path guards */
    assert(csilk_core_load_plugin(NULL, CSILK_TEST_PLUGIN_PATH) == -1);
    assert(csilk_core_load_plugin(server, NULL) == -1);
    assert(csilk_core_load_plugin(server, "") == -1);
    assert(csilk_core_load_plugin(server, "../../../tmp/evil.so") == -1);
    assert(csilk_core_load_plugin(server, "/tmp/bad\nname.so") == -1);
    assert(csilk_core_load_plugin(server, "/tmp/not_a_shared_lib.txt") == -1);

    /* ABI mismatch plugin must be rejected */
    assert(csilk_core_load_plugin(server, CSILK_TEST_BAD_ABI_PLUGIN_PATH) == -1);
    assert(csilk_core_find_plugin(server, "test-bad-abi-plugin") == NULL);

    /* Direct registration validation (ABI, NULL name, duplicate) */
    csilk_plugin_t bad_abi = {
        .info = {.abi_version = 999, .name = "bad-abi"},
        .init = dummy_static_init,
    };
    assert(csilk_core_register_plugin(server, &bad_abi) == -1);

    csilk_plugin_t null_name = {
        .info = {.abi_version = CSILK_PLUGIN_ABI_VERSION, .name = NULL},
        .init = dummy_static_init,
    };
    assert(csilk_core_register_plugin(server, &null_name) == -1);

    /* Service locator heap-ownership (strdup) & per-server isolation */
    csilk_server_t* server2 = csilk_server_new(router);
    assert(server2 != NULL);

    int  svc1_val = 42;
    int  svc2_val = 84;
    char svc_name_buf[32];
    snprintf(svc_name_buf, sizeof(svc_name_buf), "%s", "ephemeral_svc");
    assert(csilk_core_service_register(server, svc_name_buf, &svc1_val) == 0);
    /* Overwrite stack buffer to ensure service locator took an owned copy */
    memset(svc_name_buf, 'X', sizeof(svc_name_buf) - 1);

    assert(csilk_core_service_get(server, "ephemeral_svc") == &svc1_val);
    assert(csilk_core_service_get(server2, "ephemeral_svc") == NULL);
    assert(csilk_core_service_register(server2, "ephemeral_svc", &svc2_val) == 0);
    assert(csilk_core_service_get(server2, "ephemeral_svc") == &svc2_val);
    assert(csilk_core_service_register(server, "ephemeral_svc", &svc2_val) == -1);

    csilk_server_free(server2);
    csilk_server_free(server);
    csilk_router_free(router);
}

static void
test_app_env_autoload(void)
{
    /* Valid plugin path auto-loaded during csilk_app_new() */
    setenv("CSILK_PLUGIN_PATHS", CSILK_TEST_PLUGIN_PATH, 1);
    csilk_app_t* app = csilk_app_new(NULL);
    assert(app != NULL);
    /* Auto-load inside csilk_app_new succeeded; trying again returns error */
    assert(csilk_app_load_plugin(app, CSILK_TEST_PLUGIN_PATH) == -1);
    csilk_app_free(app);

    /* Mixed invalid + valid env tokens returns -1 from csilk_app_load_plugins_from_env */
    unsetenv("CSILK_PLUGIN_PATHS");
    csilk_app_t* app2 = csilk_app_new(NULL);
    assert(app2 != NULL);
    setenv("CSILK_PLUGIN_PATHS", "../../../evil.so:" CSILK_TEST_PLUGIN_PATH, 1);
    assert(csilk_app_load_plugins_from_env(app2) == -1);
    /* Still loaded the valid entry despite the rejected traversal entry */
    assert(csilk_app_load_plugin(app2, CSILK_TEST_PLUGIN_PATH) == -1);
    csilk_app_free(app2);
    unsetenv("CSILK_PLUGIN_PATHS");
}

typedef struct {
    csilk_server_t* server;
    int             tid;
} conc_arg_t;

static void*
concurrent_locator_worker(void* arg)
{
    conc_arg_t* ca = (conc_arg_t*)arg;
    char        name[32];
    snprintf(name, sizeof(name), "worker_svc_%d", ca->tid);
    assert(csilk_core_service_register(ca->server, name, ca) == 0);
    for (int i = 0; i < 100; i++) {
        assert(csilk_core_service_get(ca->server, name) == ca);
        (void)csilk_core_find_plugin(ca->server, "test-mod-plugin");
    }
    return NULL;
}

static void
test_concurrent_registry_access(void)
{
    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);
    assert(csilk_core_load_plugin(server, CSILK_TEST_PLUGIN_PATH) == 0);

    pthread_t  threads[4];
    conc_arg_t args[4];
    for (int i = 0; i < 4; i++) {
        args[i].server = server;
        args[i].tid = i;
        pthread_create(&threads[i], NULL, concurrent_locator_worker, &args[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Let csilk_server_free() auto-unload the loaded plugin and services */
    csilk_server_free(server);
    csilk_router_free(router);
}

static void
test_real_optional_plugins(void)
{
#if defined(CSILK_REAL_PLUGIN_WORKFLOW) || defined(CSILK_REAL_PLUGIN_PROTOCOL)
    /* Keep orchestrator dependencies live in the static host binary so
     * RTLD_NOW resolution for workflow/protocol plugins finds host symbols. */
    typedef void (*keep_fn_t)(void);
    static keep_fn_t const host_refs[] = {
        (keep_fn_t)csilk_ai_chat,
        (keep_fn_t)csilk_ai_chat_response_free,
        (keep_fn_t)csilk_ai_context_add,
        (keep_fn_t)csilk_ai_embeddings,
        (keep_fn_t)csilk_ai_embeddings_response_free,
        (keep_fn_t)csilk_ai_free,
        (keep_fn_t)csilk_ai_new,
        (keep_fn_t)csilk_ai_tool_dispatch,
        (keep_fn_t)csilk_ai_tool_registry_export_tools,
        (keep_fn_t)csilk_ai_tool_registry_free,
        (keep_fn_t)csilk_ai_tool_registry_new,
        (keep_fn_t)csilk_ai_tool_registry_register,
        (keep_fn_t)csilk_ai_tools_free,
        (keep_fn_t)csilk_env_get,
        (keep_fn_t)csilk_generate_uuid,
        (keep_fn_t)csilk_mq_subscribe,
        (keep_fn_t)csilk_raft_append_log,
        (keep_fn_t)csilk_raft_replay_committed,
        (keep_fn_t)csilk_raft_replicate_wait,
        (keep_fn_t)csilk_raft_set_apply_cb,
        (keep_fn_t)csilk_vector_db_search,
        (keep_fn_t)csilk_vector_db_upsert,
        (keep_fn_t)csilk_vector_search_response_free,
        (keep_fn_t)csilk_wasm_plugin_exec,
        (keep_fn_t)csilk_wasm_plugin_free,
        (keep_fn_t)csilk_wasm_plugin_load_file,
        (keep_fn_t)csilk_wf_register_tool,
    };
    volatile size_t keep_cnt = sizeof(host_refs) / sizeof(host_refs[0]);
    (void)keep_cnt;
#endif

    csilk_router_t* router = csilk_router_new();
    csilk_server_t* server = csilk_server_new(router);
    assert(server != NULL);

#ifdef CSILK_REAL_PLUGIN_AI
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_AI) == 0);
    assert(csilk_core_find_plugin(server, "csilk-ai") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-ai") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_MQ
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_MQ) == 0);
    assert(csilk_core_find_plugin(server, "csilk-mq") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-mq") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_DB
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_DB) == 0);
    assert(csilk_core_find_plugin(server, "csilk-db") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-db") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_VECTOR
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_VECTOR) == 0);
    assert(csilk_core_find_plugin(server, "csilk-vector") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-vector") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_WORKFLOW
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_WORKFLOW) == 0);
    assert(csilk_core_find_plugin(server, "csilk-workflow") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-workflow") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_PROTOCOL
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_PROTOCOL) == 0);
    assert(csilk_core_find_plugin(server, "csilk-protocol") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-protocol") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_WASM
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_WASM) == 0);
    assert(csilk_core_find_plugin(server, "csilk-wasm") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-wasm") == 0);
#endif
#ifdef CSILK_REAL_PLUGIN_BYPASS
    assert(csilk_core_load_plugin(server, CSILK_REAL_PLUGIN_BYPASS) == 0);
    assert(csilk_core_find_plugin(server, "csilk-bypass") != NULL);
    assert(csilk_core_unload_plugin(server, "csilk-bypass") == 0);
#endif

    csilk_server_free(server);
    csilk_router_free(router);
}

int
main(void)
{
    printf("Testing modular plugin loader & service locator...\n");
    test_plugin_load_init_service_and_unload();
    test_plugin_security_and_validation();
    test_app_env_autoload();
    test_concurrent_registry_access();
    test_real_optional_plugins();
    printf("test_plugin: PASS\n");
    return 0;
}
