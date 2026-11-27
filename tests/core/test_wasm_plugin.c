#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/test/test.h"

static void
test_wasm_plugin_load_invalid()
{
    printf("Testing WASM plugin invalid magic header validation...\n");

    const uint8_t        invalid_bytes[8] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(invalid_bytes, sizeof(invalid_bytes));
    assert(plugin == NULL);

    assert(csilk_wasm_plugin_load(NULL, 10) == NULL);

    printf("test_wasm_plugin_load_invalid: PASS\n");
}

static void
test_wasm_plugin_load_valid()
{
    printf("Testing WASM plugin valid header load & execution...\n");

    /* Valid WASM header: \0asm followed by version 1 */
    const uint8_t valid_wasm[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(valid_wasm, sizeof(valid_wasm));
    assert(plugin != NULL);
    assert(csilk_wasm_plugin_is_valid(plugin) == 1);

    int res = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
    /* Minimal header-only module lacks a callable body, so -1 is expected. */
    assert(res == 0 || res == -1);

    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_plugin_load_valid: PASS\n");
}

static void
test_wasm_host_api()
{
    printf("Testing WASM Host C-ABI context interaction...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);
    csilk_test_ctx_set_request(ctx, "GET", "/test");
    csilk_test_ctx_add_param(ctx, "user", "alice");

    int res = csilk_wasm_host_set_header(ctx, "X-WASM-Filter", "PASSED");
    assert(res == 0);
    const char* val = csilk_wasm_host_get_header(ctx, "X-WASM-Filter");
    assert(val != NULL);
    assert(strcmp(val, "PASSED") == 0);

    const char* param = csilk_wasm_host_get_param(ctx, "user");
    assert(param != NULL);
    assert(strcmp(param, "alice") == 0);

    res = csilk_wasm_host_set_status(ctx, 403);
    assert(res == 0);
    assert(csilk_get_status(ctx) == 403);

    csilk_test_ctx_free(ctx);
    printf("test_wasm_host_api: PASS\n");
}

static void
test_wasm_shared_mem()
{
    printf("Testing WASM zero-copy shared memory mapping...\n");

    const uint8_t        valid_wasm[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(valid_wasm, sizeof(valid_wasm));
    assert(plugin != NULL);

    char host_buf[256] = "Shared Memory Data";
    assert(csilk_wasm_plugin_map_memory(plugin, host_buf, sizeof(host_buf)) == 0);

    size_t mapped_len = 0;
    void*  mapped_ptr = csilk_wasm_host_get_mapped_buffer(plugin, &mapped_len);
    assert(mapped_ptr == host_buf);
    assert(mapped_len == sizeof(host_buf));
    assert(strcmp((const char*)mapped_ptr, "Shared Memory Data") == 0);

    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_shared_mem: PASS\n");
}

static void
test_wasm_extended_host_api(void)
{
    printf("Testing WASM extended Host APIs (body, method, path, log)...\n");

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);
    csilk_test_ctx_set_request(ctx, "POST", "/api/v1/wasm-test");
    csilk_test_ctx_set_body(ctx, "{\"ping\":\"pong\"}", 15);

    assert(strcmp(csilk_wasm_host_get_method(ctx), "POST") == 0);
    assert(strcmp(csilk_wasm_host_get_path(ctx), "/api/v1/wasm-test") == 0);

    size_t      body_len = 0;
    const char* body = csilk_wasm_host_get_body(ctx, &body_len);
    assert(body != NULL);
    assert(body_len == 15);
    assert(strcmp(body, "{\"ping\":\"pong\"}") == 0);

    assert(csilk_wasm_host_set_body(ctx, "{\"ok\":true}", 11) == 0);
    size_t      res_len = 0;
    const char* res_body = csilk_get_response_body(ctx, &res_len);
    assert(res_body != NULL && res_len == 11);
    assert(strcmp(res_body, "{\"ok\":true}") == 0);

    csilk_wasm_host_log(2, "Test message from WASM plugin");

    csilk_test_ctx_free(ctx);
    printf("test_wasm_extended_host_api: PASS\n");
}

static void
test_wasm_section_validation(void)
{
    printf("Testing WASM binary section format validation...\n");

    /* 8-byte header followed by Section 0 (Custom) len=4 payload="test" */
    uint8_t wasm_with_section[14] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 't', 'e', 's', 't'};
    csilk_wasm_plugin_t* p1 = csilk_wasm_plugin_load(wasm_with_section, sizeof(wasm_with_section));
    assert(p1 != NULL);
    csilk_wasm_plugin_free(p1);

    /* Invalid section ID 15 */
    uint8_t wasm_invalid_sec[10] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x01};
    csilk_wasm_plugin_t* p2 = csilk_wasm_plugin_load(wasm_invalid_sec, sizeof(wasm_invalid_sec));
    assert(p2 == NULL);

    /* Section length overflows buffer */
    uint8_t wasm_overflow_sec[10] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x7f};
    csilk_wasm_plugin_t* p3 = csilk_wasm_plugin_load(wasm_overflow_sec, sizeof(wasm_overflow_sec));
    assert(p3 == NULL);
    printf("test_wasm_section_validation: PASS\n");
}

static void
test_wasm_middleware_integration(void)
{
    printf("Testing WASM middleware execution and trap handling...\n");

    /* (module (memory 1) (func (export "custom_filter")) (func (export "trap_func") unreachable)) */
    static const uint8_t wasm_filter[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00,
        0x00, 0x03, 0x03, 0x02, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x1d,
        0x02, 0x0d, 0x63, 0x75, 0x73, 0x74, 0x6f, 0x6d, 0x5f, 0x66, 0x69, 0x6c, 0x74,
        0x65, 0x72, 0x00, 0x00, 0x09, 0x74, 0x72, 0x61, 0x70, 0x5f, 0x66, 0x75, 0x6e,
        0x63, 0x00, 0x01, 0x0a, 0x08, 0x02, 0x02, 0x00, 0x0b, 0x03, 0x00, 0x00, 0x0b};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(wasm_filter, sizeof(wasm_filter));
    assert(plugin != NULL);

    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);
    csilk_test_ctx_set_request(ctx, "GET", "/filter-me");

    void (*mw)(csilk_ctx_t*) = csilk_wasm_plugin_middleware(plugin, "custom_filter");
    assert(mw != NULL);
    mw(ctx);

    const char* p_hdr = csilk_get_response_header(ctx, "X-WASM-Plugin");
    assert(p_hdr != NULL);
    assert(strcmp(p_hdr, "custom_filter") == 0);

    /* Test WASM unreachable trap abort in middleware */
    csilk_ctx_t* ctx_trap = csilk_test_ctx_new();
    assert(ctx_trap != NULL);
    csilk_test_ctx_set_request(ctx_trap, "GET", "/trap");

    void (*mw_trap)(csilk_ctx_t*) = csilk_wasm_plugin_middleware(plugin, "trap_func");
    assert(mw_trap != NULL);
    mw_trap(ctx_trap);

    assert(csilk_is_aborted(ctx_trap) == 1);
    assert(csilk_get_status(ctx_trap) == 500);
    const char* err_hdr = csilk_get_response_header(ctx_trap, "X-WASM-Error");
    assert(err_hdr != NULL && strncmp(err_hdr, "CSILK_WASM_TRAP_", 16) == 0);

    csilk_test_ctx_free(ctx);
    csilk_test_ctx_free(ctx_trap);
    csilk_wasm_plugin_free(plugin);
    assert(csilk_wasm_plugin_middleware(NULL, "on_request") == NULL);
    printf("test_wasm_middleware_integration: PASS\n");
}

static void
test_wasm_multi_middleware_isolation(void)
{
    printf("Testing WASM multi-middleware isolation (no overwrite)...\n");

    /* (module (memory 1) (func (export "mw_alpha")) (func (export "mw_beta"))
     *         (func (export "mw_gamma")) (func (export "on_request"))) */
    static const uint8_t wasm_multi[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03,
        0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x2e, 0x04, 0x08,
        0x6d, 0x77, 0x5f, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x00, 0x00, 0x07, 0x6d, 0x77, 0x5f, 0x62,
        0x65, 0x74, 0x61, 0x00, 0x01, 0x08, 0x6d, 0x77, 0x5f, 0x67, 0x61, 0x6d, 0x6d, 0x61, 0x00,
        0x02, 0x0a, 0x6f, 0x6e, 0x5f, 0x72, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x00, 0x03, 0x0a,
        0x0d, 0x04, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b};
    csilk_wasm_plugin_t* p1 = csilk_wasm_plugin_load(wasm_multi, sizeof(wasm_multi));
    csilk_wasm_plugin_t* p2 = csilk_wasm_plugin_load(wasm_multi, sizeof(wasm_multi));
    assert(p1 != NULL && p2 != NULL);

    /* Register all 4 middlewares BEFORE invoking any of them. */
    csilk_handler_t mw1 = csilk_wasm_plugin_middleware(p1, "mw_alpha");
    csilk_handler_t mw2 = csilk_wasm_plugin_middleware(p1, "mw_beta");
    csilk_handler_t mw3 = csilk_wasm_plugin_middleware(p2, "mw_gamma");
    csilk_handler_t mw4 = csilk_wasm_plugin_middleware(p2, NULL); /* defaults to on_request */

    assert(mw1 != NULL && mw2 != NULL && mw3 != NULL && mw4 != NULL);
    assert(mw1 != mw2 && mw1 != mw3 && mw1 != mw4);
    assert(mw2 != mw3 && mw2 != mw4 && mw3 != mw4);

    const char*     expected[] = {"mw_alpha", "mw_beta", "mw_gamma", "on_request"};
    csilk_handler_t mws[] = {mw1, mw2, mw3, mw4};

    /* Invoke in reverse order to prove earlier registrations were not overwritten. */
    for (int i = 3; i >= 0; i--) {
        csilk_ctx_t* ctx = csilk_test_ctx_new();
        assert(ctx != NULL);
        csilk_test_ctx_set_request(ctx, "GET", "/multi");
        mws[i](ctx);
        const char* hdr = csilk_get_response_header(ctx, "X-WASM-Plugin");
        assert(hdr != NULL && strcmp(hdr, expected[i]) == 0);
        csilk_test_ctx_free(ctx);
    }

    csilk_wasm_plugin_free(p1);
    csilk_wasm_plugin_free(p2);
    printf("test_wasm_multi_middleware_isolation: PASS\n");
}

static void
test_wasm_middleware_saturation_and_reclamation(void)
{
    printf("Testing WASM 16-slot thunk saturation, reclamation on free, and post-free safety...\n");

    /* (module (memory 1) (func (export "fn0")) (func (export "fn1"))
     *         (func (export "fn2")) (func (export "fn3"))) */
    static const uint8_t wasm_slot[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x19,
        0x04, 0x03, 0x66, 0x6e, 0x30, 0x00, 0x00, 0x03, 0x66, 0x6e, 0x31, 0x00, 0x01, 0x03,
        0x66, 0x6e, 0x32, 0x00, 0x02, 0x03, 0x66, 0x6e, 0x33, 0x00, 0x03, 0x0a, 0x0d, 0x04,
        0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b};
    csilk_wasm_plugin_t* plugins[4] = {NULL};
    csilk_handler_t      slots[16] = {NULL};

    for (int p = 0; p < 4; p++) {
        plugins[p] = csilk_wasm_plugin_load(wasm_slot, sizeof(wasm_slot));
        assert(plugins[p] != NULL);
        for (int k = 0; k < 4; k++) {
            char fname[16];
            snprintf(fname, sizeof(fname), "fn%d", k);
            int idx = p * 4 + k;
            slots[idx] = csilk_wasm_plugin_middleware(plugins[p], fname);
            assert(slots[idx] != NULL);
        }
    }

    /* Pool is now full (16 slots); 17th registration must return NULL, not overwrite. */
    assert(csilk_wasm_plugin_middleware(plugins[0], "fn0") == NULL);

    /* Free plugin 1 (held slots 4..7): all 4 slots must be reclaimed. */
    csilk_handler_t stale_mw = slots[5];
    csilk_wasm_plugin_free(plugins[1]);
    plugins[1] = NULL;

    /* Calling a stale thunk after csilk_wasm_plugin_free must be a safe no-op (no UAF). */
    csilk_ctx_t* stale_ctx = csilk_test_ctx_new();
    assert(stale_ctx != NULL);
    csilk_test_ctx_set_request(stale_ctx, "GET", "/stale");
    stale_mw(stale_ctx);
    assert(csilk_get_response_header(stale_ctx, "X-WASM-Plugin") == NULL);
    csilk_test_ctx_free(stale_ctx);

    /* Re-register 4 middlewares into the reclaimed slots, then saturate again. */
    csilk_wasm_plugin_t* p_new = csilk_wasm_plugin_load(wasm_slot, sizeof(wasm_slot));
    assert(p_new != NULL);
    for (int k = 0; k < 4; k++) {
        char fname[16];
        snprintf(fname, sizeof(fname), "fn%d", k);
        csilk_handler_t mw = csilk_wasm_plugin_middleware(p_new, fname);
        assert(mw != NULL);

        csilk_ctx_t* ctx = csilk_test_ctx_new();
        assert(ctx != NULL);
        csilk_test_ctx_set_request(ctx, "GET", "/reclaimed");
        mw(ctx);
        const char* hdr = csilk_get_response_header(ctx, "X-WASM-Plugin");
        assert(hdr != NULL && strcmp(hdr, fname) == 0);
        csilk_test_ctx_free(ctx);
    }
    assert(csilk_wasm_plugin_middleware(p_new, "fn0") == NULL);

    csilk_wasm_plugin_free(p_new);
    csilk_wasm_plugin_free(plugins[0]);
    csilk_wasm_plugin_free(plugins[2]);
    csilk_wasm_plugin_free(plugins[3]);
    printf("test_wasm_middleware_saturation_and_reclamation: PASS\n");
}

int
main()
{
    test_wasm_plugin_load_invalid();
    test_wasm_plugin_load_valid();
    test_wasm_host_api();
    test_wasm_shared_mem();
    test_wasm_extended_host_api();
    test_wasm_section_validation();
    test_wasm_middleware_integration();
    test_wasm_multi_middleware_isolation();
    test_wasm_middleware_saturation_and_reclamation();
    printf("All WASM Plugin tests passed successfully!\n");
    return 0;
}
