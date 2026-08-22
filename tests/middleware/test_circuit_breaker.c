/**
 * @file test_circuit_breaker.c
 * @brief Unit tests for circuit breaker state machine and middleware.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/middleware.h"
#include "csilk/core/sync.h"
#include "csilk/core/sys_io.h"
#include "csilk/test/test.h"

static void
test_cb_new_null_config(void)
{
    printf("Testing csilk_circuit_breaker_new with NULL config (uses defaults)...\n");
    csilk_circuit_breaker_t* cb = csilk_circuit_breaker_new(NULL);
    assert(cb != NULL);
    csilk_circuit_breaker_free(cb);
    printf("  passed\n");
}

static void
test_cb_state_transitions(void)
{
    printf("Testing circuit breaker state transitions...\n");
    csilk_circuit_breaker_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.failure_threshold = 3;
    cfg.recovery_timeout_ms = 1000;

    csilk_circuit_breaker_t* cb = csilk_circuit_breaker_new(&cfg);
    assert(cb != NULL);

    /* Initial state: CLOSED */
    int state = csilk_circuit_breaker_get_state(cb);
    assert(state == 0); /* CLOSED */

    /* Record failures to trip the breaker */
    csilk_circuit_breaker_record_failure(cb);
    assert(csilk_circuit_breaker_get_state(cb) == 0); /* still closed (threshold not reached) */

    csilk_circuit_breaker_record_failure(cb);
    assert(csilk_circuit_breaker_get_state(cb) == 0); /* still closed */

    csilk_circuit_breaker_record_failure(cb);
    /* State should now be OPEN after reaching threshold */
    state = csilk_circuit_breaker_get_state(cb);
    assert(state == 1); /* OPEN */

    csilk_circuit_breaker_free(cb);
    printf("  passed\n");
}

static void
test_cb_reset_on_success(void)
{
    printf("Testing circuit breaker reset on success...\n");
    csilk_circuit_breaker_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.failure_threshold = 5;
    cfg.recovery_timeout_ms = 1000;

    csilk_circuit_breaker_t* cb = csilk_circuit_breaker_new(&cfg);
    assert(cb != NULL);

    /* Record some failures then a success should reset counter */
    csilk_circuit_breaker_record_failure(cb);
    csilk_circuit_breaker_record_success(cb);
    /* Should still be CLOSED, counter reset */
    assert(csilk_circuit_breaker_get_state(cb) == 0);

    csilk_circuit_breaker_free(cb);
    printf("  passed\n");
}

static void
test_cb_null_ctx(void)
{
    printf("Testing csilk_circuit_breaker_middleware with NULL ctx...\n");
    csilk_circuit_breaker_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.failure_threshold = 3;
    cfg.recovery_timeout_ms = 1000;
    csilk_circuit_breaker_t* cb = csilk_circuit_breaker_new(&cfg);
    assert(cb != NULL);

    /* NULL ctx should not crash */
    csilk_circuit_breaker_middleware(NULL, cb);

    csilk_circuit_breaker_free(cb);
    printf("  passed\n");
}

static void
test_cb_null_cb(void)
{
    printf("Testing csilk_circuit_breaker_middleware with NULL cb...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    assert(ctx != NULL);

    /* NULL cb should not crash */
    csilk_circuit_breaker_middleware(ctx, NULL);

    csilk_test_ctx_free(ctx);
    printf("  passed\n");
}

static void
test_cb_free_null(void)
{
    printf("Testing csilk_circuit_breaker_free with NULL...\n");
    csilk_circuit_breaker_free(NULL);
    printf("  passed\n");
}

static void
test_cb_get_state_null(void)
{
    printf("Testing csilk_circuit_breaker_get_state with NULL...\n");
    int state = csilk_circuit_breaker_get_state(NULL);
    assert(state == 0);
    printf("  passed\n");
}

static void
test_cb_record_failure_null(void)
{
    printf("Testing csilk_circuit_breaker_record_failure with NULL...\n");
    csilk_circuit_breaker_record_failure(NULL);
    printf("  passed\n");
}

static void
test_cb_record_success_null(void)
{
    printf("Testing csilk_circuit_breaker_record_success with NULL...\n");
    csilk_circuit_breaker_record_success(NULL);
    printf("  passed\n");
}

static void
test_cb_default_config(void)
{
    printf("Testing circuit breaker with default config...\n");
    csilk_circuit_breaker_t* cb = csilk_circuit_breaker_new(NULL);
    assert(cb != NULL);

    int state = csilk_circuit_breaker_get_state(cb);
    assert(state == 0); /* CLOSED */

    csilk_circuit_breaker_free(cb);
    printf("  passed\n");
}

int
main(void)
{
    test_cb_new_null_config();
    test_cb_state_transitions();
    test_cb_reset_on_success();
    test_cb_null_ctx();
    test_cb_null_cb();
    test_cb_free_null();
    test_cb_get_state_null();
    test_cb_record_failure_null();
    test_cb_record_success_null();
    test_cb_default_config();

    printf("All test_circuit_breaker tests passed successfully!\n");
    return 0;
}
