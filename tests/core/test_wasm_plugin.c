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
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    assert(plugin == NULL);
    assert(errno == ENOTSUP);
    printf("test_wasm_plugin_load_valid (fail-closed): PASS\n");
    return;
#else
    assert(plugin != NULL);
    assert(csilk_wasm_plugin_is_valid(plugin) == 1);

    int res = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
    assert(res == 0);

    csilk_wasm_plugin_free(plugin);
    printf("test_wasm_plugin_load_valid: PASS\n");
#endif
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
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    assert(plugin == NULL);
    assert(errno == ENOTSUP);
    printf("test_wasm_shared_mem (fail-closed): PASS\n");
    return;
#else
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
#endif
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

#if CSILK_ENABLE_EXPERIMENTAL_STUBS
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
#endif
    printf("test_wasm_section_validation: PASS\n");
}

static void
test_wasm_middleware_integration(void)
{
    printf("Testing WASM middleware execution and fuel trap...\n");

#if CSILK_ENABLE_EXPERIMENTAL_STUBS
    const uint8_t        valid_wasm[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(valid_wasm, sizeof(valid_wasm));
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

    /* Test Fuel exhaustion abort in middleware */
    csilk_ctx_t* ctx_trap = csilk_test_ctx_new();
    assert(ctx_trap != NULL);
    csilk_test_ctx_set_request(ctx_trap, "GET", "/loop");

    void (*mw_loop)(csilk_ctx_t*) = csilk_wasm_plugin_middleware(plugin, "infinite_loop");
    mw_loop(ctx_trap);

    assert(csilk_is_aborted(ctx_trap) == 1);
    assert(csilk_get_status(ctx_trap) == 500);
    const char* err_hdr = csilk_get_response_header(ctx_trap, "X-WASM-Error");
    assert(err_hdr != NULL && strcmp(err_hdr, "TRAP_FUEL_EXHAUSTED") == 0);

    csilk_test_ctx_free(ctx);
    csilk_test_ctx_free(ctx_trap);
    csilk_wasm_plugin_free(plugin);
#endif
    printf("test_wasm_middleware_integration: PASS\n");
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
    printf("All WASM Plugin tests passed successfully!\n");
    return 0;
}
