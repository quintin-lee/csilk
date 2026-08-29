/**
 * @file ctx_async.c
 * @brief Managed asynchronous operation lifecycle, timeout management, and generation safety.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include "csilk/core/async.h"
#include "csilk/core/internal.h"
#include "csilk/core/response.h"
#include "../internal/srv_impl.h"
#include "../internal/srv_internal.h"
#include "ctx_internal.h"

typedef struct {
    csilk_async_op_t* op;
    void*             result;
} csilk_async_dispatch_payload_t;

static void
_csilk_async_timer_cb(csilk_io_timer_t* handle)
{
    csilk_async_op_t* op = (csilk_async_op_t*)handle->data;
    if (!op) {
        return;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &op->completed, &expected, 1, memory_order_acq_rel, memory_order_relaxed)) {
        return; /* Already completed by worker or user callback */
    }

    csilk_ctx_t*    c = op->ctx;
    csilk_client_t* client = c ? (csilk_client_t*)c->_internal_client : NULL;

    /* Verify anti-ABA generation validity */
    if (client && client->generation == op->generation && c->request_seq == op->request_seq) {
        if (op->on_timeout) {
            op->on_timeout(c);
        } else {
            csilk_status(c, CSILK_STATUS_GATEWAY_TIMEOUT);
            csilk_string(c, CSILK_STATUS_GATEWAY_TIMEOUT, "Gateway Timeout");
            _csilk_send_response(c);
        }
    }

    csilk_io_timer_stop(&op->timer);
    if (client) {
        csilk_client_unref(client);
    }
    free(op);
}

csilk_async_op_t*
csilk_async_op_begin(csilk_ctx_t*           c,
                     uint64_t               timeout_ms,
                     csilk_async_cb         on_complete,
                     csilk_async_timeout_cb on_timeout,
                     void*                  user_data)
{
    if (!c) {
        return NULL;
    }

    csilk_async_op_t* op = (csilk_async_op_t*)calloc(1, sizeof(csilk_async_op_t));
    if (!op) {
        return NULL;
    }

    op->ctx = c;
    op->on_complete = on_complete;
    op->on_timeout = on_timeout;
    op->user_data = user_data;
    op->request_seq = c->request_seq;
    atomic_init(&op->completed, 0);

    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client) {
        op->generation = client->generation;
        csilk_client_ref(client);
    }

    c->is_async = 1;

    if (timeout_ms > 0) {
        csilk_io_loop_t* loop = _csilk_ctx_loop(c);
        if (loop) {
            csilk_io_timer_init(loop, &op->timer);
            op->timer.data = op;
            csilk_io_timer_start(&op->timer, _csilk_async_timer_cb, timeout_ms, 0);
            op->timer_armed = 1;
        }
    }

    return op;
}

static void
_csilk_async_complete_dispatch_cb(void* arg)
{
    csilk_async_dispatch_payload_t* p = (csilk_async_dispatch_payload_t*)arg;
    if (!p) {
        return;
    }

    csilk_async_op_t* op = p->op;
    void*             result = p->result;
    free(p);

    if (!op) {
        return;
    }

    csilk_ctx_t*    c = op->ctx;
    csilk_client_t* client = c ? (csilk_client_t*)c->_internal_client : NULL;

    if (op->timer_armed) {
        csilk_io_timer_stop(&op->timer);
    }

    /* Verify anti-ABA generation validity */
    if (client && client->generation == op->generation && c->request_seq == op->request_seq) {
        if (op->on_complete) {
            op->on_complete(c, result);
        }
    }

    if (client) {
        csilk_client_unref(client);
    }
    free(op);
}

int
csilk_async_op_complete(csilk_async_op_t* op, void* result)
{
    if (!op || !op->ctx) {
        return -1;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &op->completed, &expected, 1, memory_order_acq_rel, memory_order_relaxed)) {
        return -1; /* Already completed or timed out */
    }

    csilk_async_dispatch_payload_t* payload =
        (csilk_async_dispatch_payload_t*)malloc(sizeof(csilk_async_dispatch_payload_t));
    if (!payload) {
        return -1;
    }
    payload->op = op;
    payload->result = result;

    csilk_dispatch(op->ctx, _csilk_async_complete_dispatch_cb, payload);
    return 0;
}

int
csilk_async_op_cancel(csilk_async_op_t* op)
{
    if (!op) {
        return -1;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &op->completed, &expected, 1, memory_order_acq_rel, memory_order_relaxed)) {
        return -1;
    }

    if (op->timer_armed) {
        csilk_io_timer_stop(&op->timer);
    }

    csilk_client_t* client = op->ctx ? (csilk_client_t*)op->ctx->_internal_client : NULL;
    if (client) {
        csilk_client_unref(client);
    }

    free(op);
    return 0;
}
