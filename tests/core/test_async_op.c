/**
 * @file test_async_op.c
 * @brief Unit tests for managed asynchronous operation csilk_async_op_t.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/async.h"
#include "csilk/test/test.h"
#include "core/ctx/ctx_internal.h"

static int g_async_completed = 0;
static int g_async_timeout = 0;

static void
_test_complete_cb(csilk_ctx_t* c, void* result)
{
    (void)c;
    int* val = (int*)result;
    if (val && *val == 42) {
        g_async_completed++;
    }
}

static void
_test_timeout_cb(csilk_ctx_t* c)
{
    (void)c;
    g_async_timeout++;
}

static void
test_async_op_basic_lifecycle(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    g_async_completed = 0;
    csilk_async_op_t* op = csilk_async_op_begin(c, 0, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op != NULL);
    assert(c->is_async == 1);

    int result = 42;
    int rc = csilk_async_op_complete(op, &result);
    assert(rc == 0);

    /* Second complete attempt should fail */
    assert(csilk_async_op_complete(op, &result) == -1);

    csilk_test_ctx_free(c);
    printf("✓ test_async_op_basic_lifecycle passed\n");
}

static void
test_async_op_cancellation(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    csilk_async_op_t* op = csilk_async_op_begin(c, 0, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op != NULL);

    int rc = csilk_async_op_cancel(op);
    assert(rc == 0);

    /* Subsequent cancel or complete should fail */
    assert(csilk_async_op_cancel(op) == -1);

    csilk_test_ctx_free(c);
    printf("✓ test_async_op_cancellation passed\n");
}

static void
test_async_op_null_safety(void)
{
    assert(csilk_async_op_begin(NULL, 0, NULL, NULL, NULL) == NULL);
    assert(csilk_async_op_complete(NULL, NULL) == -1);
    assert(csilk_async_op_cancel(NULL) == -1);
    printf("✓ test_async_op_null_safety passed\n");
}

int
main(void)
{
    printf("=== Running csilk_async_op_t unit tests ===\n");
    test_async_op_basic_lifecycle();
    test_async_op_cancellation();
    test_async_op_null_safety();
    printf("All csilk_async_op_t tests passed successfully!\n");
    return 0;
}
