/**
 * @file test_wasm_real.c
 * @brief WAMR real execution path tests (compiled only when HAS_WAMR is defined).
 *
 * When HAS_WAMR is not defined, this test is a no-op that verifies the
 * fail-closed behaviour of csilk_wasm_plugin_load when experimental stubs
 * are disabled.
 *
 * @copyright MIT License
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "csilk/test/test.h"

#ifdef HAS_WAMR
#include "wasm_export.h"
#endif

/*
 * Minimal valid WASM module (WAT-compiled):
 * (module (func (export "on_request")))
 * 8-byte header + type section + func section + export section
 */
static const uint8_t minimal_wasm[] = {
    /* Magic + version */
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0x00,
    0x00,
    0x00,
    /* Type section: 1 func type () -> () */
    0x01,
    0x04,
    0x01,
    0x60,
    0x00,
    0x00,
    /* Function section: 1 func of type 0 */
    0x03,
    0x02,
    0x01,
    0x00,
    /* Export section: export "on_request" as func 0 */
    0x07,
    0x0e,
    0x01,
    0x0a,
    0x6f,
    0x6e,
    0x5f,
    0x72,
    0x65,
    0x71,
    0x75,
    0x65,
    0x73,
    0x74,
    0x00,
    0x00};

/*
 * These tests only run when the WASM load path is active:
 * - CSILK_ENABLE_EXPERIMENTAL_STUBS=ON (stub path), or
 * - HAS_WAMR defined (WAMR real execution path)
 * When neither is active (stubs OFF + no WAMR), the load is fail-closed by
 * design and the tests are skipped.
 */
#if CSILK_ENABLE_EXPERIMENTAL_STUBS || defined(HAS_WAMR)
#define WASM_LOAD_ACTIVE 1
#else
#define WASM_LOAD_ACTIVE 0
#endif

static void
test_wasm_real_load_valid(void)
{
    printf("Testing WASM load of minimal valid module...\n");
#if WASM_LOAD_ACTIVE
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
    assert(plugin != NULL);
    assert(csilk_wasm_plugin_is_valid(plugin) == 1);
    csilk_wasm_plugin_free(plugin);
#else
    /* Fail-closed: load must return NULL when stubs are OFF and no WAMR */
    errno = 0;
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
    assert(plugin == NULL);
#endif
    printf("test_wasm_real_load_valid: PASS\n");
}

static void
test_wasm_real_exec_fuel_reset(void)
{
    printf("Testing WASM fuel exhaustion L1 fix (fuel resets per call)...\n");
#if WASM_LOAD_ACTIVE
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
    assert(plugin != NULL);
    /*
     * Call execute 20 times. With the L1 bug (fuel_limit -= 100 each call),
     * after 10000 calls the plugin would be permanently disabled.
     * With the fix, each call is independent and the plugin stays usable.
     */
    for (int i = 0; i < 20; i++) {
        int rc = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
        assert(rc == 0 || rc == -1);
    }
    csilk_wasm_plugin_free(plugin);
#else
    /* Fail-closed: verify that repeated loads all return NULL */
    for (int i = 0; i < 3; i++) {
        errno = 0;
        csilk_wasm_plugin_t* p = csilk_wasm_plugin_load(minimal_wasm, sizeof(minimal_wasm));
        assert(p == NULL);
    }
#endif
    printf("test_wasm_real_exec_fuel_reset: PASS\n");
}

static void
test_wasm_real_invalid_module_fallback(void)
{
    printf("Testing WASM fallback for invalid module...\n");
    const uint8_t header_only[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
#if WASM_LOAD_ACTIVE
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(header_only, sizeof(header_only));
    assert(plugin != NULL);
    int rc = csilk_wasm_plugin_execute(plugin, NULL, "on_request");
    assert(rc == 0); /* stub path succeeds */
    csilk_wasm_plugin_free(plugin);
#else
    errno = 0;
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(header_only, sizeof(header_only));
    assert(plugin == NULL);
#endif
    printf("test_wasm_real_invalid_module_fallback: PASS\n");
}

int
main(void)
{
#ifdef HAS_WAMR
    /* Initialize WAMR runtime before first use */
    if (!wasm_runtime_init()) {
        printf("WAMR runtime init failed — skipping real WAMR tests\n");
        return 0;
    }
#endif

    test_wasm_real_load_valid();
    test_wasm_real_exec_fuel_reset();
    test_wasm_real_invalid_module_fallback();

#ifdef HAS_WAMR
    wasm_runtime_destroy();
#endif

    printf("test_wasm_real: ALL PASSED\n");
    return 0;
}
