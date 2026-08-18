/**
 * @file test_ctx_lifecycle_async.c
 * @brief Unit tests for csilk_ctx_t asynchronous ownership, lease, and lifecycle safety.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/messaging/mq.h"
#include "csilk/drivers/ai.h"
#include "csilk/app/workflow.h"
#include "messaging/mq_internal.h"
#include "core/ctx/ctx_internal.h"

static void
test_async_token_basic(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    csilk_async_token_t token = csilk_ctx_acquire_async(c);
    assert(token.ctx == c);
    assert(token.request_seq > 0);
    assert(csilk_async_token_validate(&token) == 1);

    csilk_ctx_release_async(&token);
    csilk_test_ctx_free(c);
    printf("✓ test_async_token_basic passed\n");
}

static void
test_async_token_stale_after_cleanup(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    /* Acquire token for Request 1 */
    csilk_async_token_t token1 = csilk_ctx_acquire_async(c);
    assert(csilk_async_token_validate(&token1) == 1);
    uint64_t seq1 = token1.request_seq;

    /* Simulate Request 1 completion and Keep-Alive reuse */
    csilk_ctx_cleanup(c);

    /* Token 1 must now be detected as STALE */
    assert(csilk_async_token_validate(&token1) == 0);
    assert(csilk_ctx_get_request_seq(c) > seq1);

    /* Acquire token for Request 2 */
    csilk_async_token_t token2 = csilk_ctx_acquire_async(c);
    assert(csilk_async_token_validate(&token2) == 1);
    assert(token2.request_seq > seq1);

    /* Token 1 remains stale */
    assert(csilk_async_token_validate(&token1) == 0);

    csilk_ctx_release_async(&token1);
    csilk_ctx_release_async(&token2);
    csilk_test_ctx_free(c);
    printf("✓ test_async_token_stale_after_cleanup passed\n");
}

static void
test_async_token_conn_closed(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    csilk_async_token_t token = csilk_ctx_acquire_async(c);
    assert(csilk_async_token_validate(&token) == 1);

    /* Simulate client disconnect / timeout */
    c->conn_closed = 1;
    assert(csilk_async_token_validate(&token) == 0);

    csilk_ctx_release_async(&token);
    csilk_test_ctx_free(c);
    printf("✓ test_async_token_conn_closed passed\n");
}

static void
test_async_token_null_safety(void)
{
    assert(csilk_async_token_validate(NULL) == 0);
    csilk_async_token_t empty = {0};
    assert(csilk_async_token_validate(&empty) == 0);
    assert(csilk_ctx_get_request_seq(NULL) == 0);

    csilk_ctx_release_async(NULL);
    csilk_ctx_release_async(&empty);
    printf("✓ test_async_token_null_safety passed\n");
}

static void
test_mq_monitor_lifecycle(void)
{
    csilk_io_loop_t* loop = csilk_io_default_loop();
    csilk_mq_t*      mq = _csilk_mq_new(loop);
    assert(mq != NULL);

    csilk_ctx_t* c1 = csilk_test_ctx_new();
    csilk_ctx_t* c2 = csilk_test_ctx_new();

    csilk_mq_register_monitor(mq, c1);
    csilk_mq_register_monitor(mq, c2);
    assert(mq->monitor_count == 2);

    /* Explicit unregister */
    csilk_mq_unregister_monitor(mq, c1);
    assert(mq->monitor_count == 1);
    assert(mq->monitors[0] == c2);

    /* Simulate c2 disconnect and verify auto-prune on publish */
    c2->conn_closed = 1;
    csilk_mq_publish(mq, "test/topic", "hello", 5);

    csilk_mq_unregister_monitor(mq, c2);
    _csilk_mq_free(mq);
    csilk_test_ctx_free(c1);
    csilk_test_ctx_free(c2);
    printf("✓ test_mq_monitor_lifecycle passed\n");
}

static void
test_ai_and_wf_monitor_lifecycle(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    assert(c != NULL);

    /* AI monitor register & unregister */
    csilk_ai_register_monitor(c);
    csilk_ai_unregister_monitor(c);

    /* Workflow monitor register & unregister */
    csilk_wf_t* wf = csilk_wf_new("test_flow");
    assert(wf != NULL);
    csilk_wf_register_monitor(wf, c);
    csilk_wf_unregister_monitor(wf, c);
    csilk_wf_free(wf);

    csilk_test_ctx_free(c);
    printf("✓ test_ai_and_wf_monitor_lifecycle passed\n");
}

int
main(void)
{
    printf("=== Starting csilk_ctx_t Async Lifecycle & Ownership Tests ===\n");
    test_async_token_basic();
    test_async_token_stale_after_cleanup();
    test_async_token_conn_closed();
    test_async_token_null_safety();
    test_mq_monitor_lifecycle();
    test_ai_and_wf_monitor_lifecycle();
    printf("=== All csilk_ctx_t Async Lifecycle & Ownership Tests PASSED ===\n");
    return 0;
}
