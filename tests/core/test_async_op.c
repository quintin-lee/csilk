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
#include "core/internal/srv_internal.h"
#include "csilk/http/h2.h"

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
    assert(g_async_completed == 1);

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

static void
test_async_op_h2_stream_lifecycle(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    csilk_ctx_t* stream = csilk_h2_get_or_create_stream(&client, 1);
    assert(stream != NULL);
    assert(stream->stream_ref == 1);
    assert(stream->stream_closed == 0);

    g_async_completed = 0;
    csilk_async_op_t* op =
        csilk_async_op_begin(stream, 0, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op != NULL);
    assert(stream->stream_ref == 2);

    int result = 42;
    int rc = csilk_async_op_complete(op, &result);
    assert(rc == 0);
    assert(g_async_completed == 1);
    assert(stream->stream_ref == 1);

    /* Close stream from nghttp2 */
    rc = csilk_h2_remove_stream(&client, 1);
    assert(rc == 0);
    assert(client.h2_stream_map.count == 0);
    assert(client.h2_stream_map.pool_count == 1);

    csilk_h2_free_streams(&client);
    printf("✓ test_async_op_h2_stream_lifecycle passed\n");
}

static void
test_async_op_h2_stream_close_before_complete(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    csilk_ctx_t* stream = csilk_h2_get_or_create_stream(&client, 3);
    assert(stream != NULL);
    assert(stream->stream_ref == 1);

    g_async_completed = 0;
    csilk_async_op_t* op =
        csilk_async_op_begin(stream, 0, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op != NULL);
    assert(stream->stream_ref == 2);

    /* Simulate RST_STREAM / client disconnect while async operation is pending */
    int rc = csilk_h2_remove_stream(&client, 3);
    assert(rc == 0);
    assert(client.h2_stream_map.count == 0);
    /* Stream should NOT be in free_list yet because op holds ref */
    assert(client.h2_stream_map.pool_count == 0);
    assert(stream->stream_closed == 1);
    assert(stream->stream_ref == 1);

    /* Complete async op now */
    int result = 42;
    rc = csilk_async_op_complete(op, &result);
    assert(rc == 0);
    /* on_complete should NOT be called because stream was closed */
    assert(g_async_completed == 0);
    /* Stream is now recycled to pool */
    assert(client.h2_stream_map.pool_count == 1);

    csilk_h2_free_streams(&client);
    printf("✓ test_async_op_h2_stream_close_before_complete passed\n");
}

static void
test_async_op_timeout_cancellation_active_timer(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    /* Begin async op with 5000ms timeout on default loop */
    csilk_async_op_t* op = csilk_async_op_begin(c, 5000, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op != NULL);
    assert(op->timer_armed == 1);
    assert(op->ref_count == 2); /* 1 for user, 1 for timer */

    /* Cancel op immediately */
    int rc = csilk_async_op_cancel(op);
    assert(rc == 0);

    /* Run event loop ticks to let timer close callback finish */
    for (int i = 0; i < 5; i++) {
        csilk_io_run((csilk_io_loop_t*)uv_default_loop(), CSILK_IO_RUN_NOWAIT);
    }

    csilk_test_ctx_free(c);
    printf("✓ test_async_op_timeout_cancellation_active_timer passed\n");
}

static void
test_async_op_h2_aba_protection(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    /* Generation 1 of stream 5 */
    csilk_ctx_t* stream_gen1 = csilk_h2_get_or_create_stream(&client, 5);
    assert(stream_gen1 != NULL);
    uint32_t gen1 = stream_gen1->stream_gen;

    g_async_completed = 0;
    csilk_async_op_t* op_gen1 =
        csilk_async_op_begin(stream_gen1, 0, _test_complete_cb, _test_timeout_cb, NULL);
    assert(op_gen1 != NULL);
    assert(op_gen1->stream_gen == gen1);

    /* Close stream from nghttp2 (removes from bucket map) */
    csilk_h2_remove_stream(&client, 5);

    /* Now drop op ref so it returns to free_list pool */
    int result = 42;
    csilk_async_op_complete(op_gen1, &result);
    assert(g_async_completed == 0); /* Suppressed because stream was closed */
    assert(client.h2_stream_map.pool_count == 1);

    /* Recreate stream 5 from pool - will receive Generation 2 */
    csilk_ctx_t* stream_gen2 = csilk_h2_get_or_create_stream(&client, 5);
    assert(stream_gen2 == stream_gen1);
    assert(stream_gen2->stream_gen > gen1);

    csilk_h2_free_streams(&client);
    printf("✓ test_async_op_h2_aba_protection passed\n");
}

int
main(void)
{
    printf("=== Running csilk_async_op_t unit tests ===\n");
    test_async_op_basic_lifecycle();
    test_async_op_cancellation();
    test_async_op_null_safety();
    test_async_op_h2_stream_lifecycle();
    test_async_op_h2_stream_close_before_complete();
    test_async_op_timeout_cancellation_active_timer();
    test_async_op_h2_aba_protection();
    printf("All csilk_async_op_t tests passed successfully!\n");
    return 0;
}
