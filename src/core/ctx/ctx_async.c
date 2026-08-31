/**
 * @file ctx_async.c
 * @brief Managed asynchronous operation lifecycle, timeout management, and generation safety.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include "csilk/core/ctx/async.h"
#include "csilk/core/internal.h"
#include "csilk/core/ctx/response.h"
#include "../internal/srv_impl.h"
#include "../internal/srv_internal.h"
#include "ctx_internal.h"

typedef struct {
    csilk_async_op_t* op;
    void*             result;
} csilk_async_dispatch_payload_t;

static void
_csilk_async_op_unref(csilk_async_op_t* op)
{
    if (!op) {
        return;
    }
    if (atomic_fetch_sub_explicit(&op->ref_count, 1, memory_order_acq_rel) > 1) {
        return;
    }

    /* ref_count reached 0 — release stream and connection references and free op */
    csilk_ctx_t*    c = op->ctx;
    csilk_client_t* client = c ? (csilk_client_t*)c->_internal_client : NULL;
    _csilk_stream_unref(c);
    if (client) {
        csilk_client_unref(client);
    }
    free(op);
}

static void
_csilk_async_timer_close_cb(csilk_io_handle_t* handle)
{
    csilk_async_op_t* op = (csilk_async_op_t*)handle->data;
    if (op) {
        _csilk_async_op_unref(op);
    }
}

static void
_csilk_async_op_disarm_timer(csilk_async_op_t* op)
{
    if (!op || !op->timer_armed) {
        return;
    }
    op->timer_armed = 0;
    csilk_io_timer_stop(&op->timer);
    if (!op->timer_closed) {
        op->timer_closed = 1;
        csilk_io_close((csilk_io_handle_t*)&op->timer, _csilk_async_timer_close_cb);
    }
}

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
        return; /* Already completed or cancelled */
    }

    csilk_ctx_t*    c = op->ctx;
    csilk_client_t* client = c ? (csilk_client_t*)c->_internal_client : NULL;

    /* Verify anti-ABA generation validity, request sequence, stream generation, and stream not closed */
    if (!client ||
        (client->generation == op->generation && c->request_seq == op->request_seq &&
         (c->h2_stream_owner == NULL || c->stream_gen == op->stream_gen) && !c->stream_closed)) {
        if (op->on_timeout) {
            op->on_timeout(c);
        } else {
            csilk_status(c, CSILK_STATUS_GATEWAY_TIMEOUT);
            csilk_string(c, CSILK_STATUS_GATEWAY_TIMEOUT, "Gateway Timeout");
            _csilk_send_response(c);
        }
    }

    _csilk_async_op_disarm_timer(op);
    /* Drop caller's initial reference */
    _csilk_async_op_unref(op);
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
    op->stream_gen = (uint32_t)c->stream_gen;
    atomic_init(&op->completed, 0);
    atomic_init(&op->ref_count, 1); /* User reference */

    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client) {
        op->generation = client->generation;
        csilk_client_ref(client);
    }
    _csilk_stream_ref(c);

    c->is_async = 1;

    if (timeout_ms > 0) {
        csilk_io_loop_t* loop = _csilk_ctx_loop(c);
        if (loop) {
            csilk_io_timer_init(loop, &op->timer);
            op->timer.data = op;
            atomic_fetch_add_explicit(
                &op->ref_count, 1, memory_order_relaxed); /* Timer reference */
            op->timer_armed = 1;
            csilk_io_timer_start(&op->timer, _csilk_async_timer_cb, timeout_ms, 0);
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

    _csilk_async_op_disarm_timer(op);

    /* Verify anti-ABA generation validity, request sequence, stream generation, and stream not closed */
    if (!client ||
        (client->generation == op->generation && c->request_seq == op->request_seq &&
         (c->h2_stream_owner == NULL || c->stream_gen == op->stream_gen) && !c->stream_closed)) {
        if (op->on_complete) {
            op->on_complete(c, result);
        }
    }

    /* Release dispatch reference */
    _csilk_async_op_unref(op);
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

    csilk_ctx_t*    c = op->ctx;
    csilk_client_t* client = c ? (csilk_client_t*)c->_internal_client : NULL;

    /* If on owner worker thread or standalone mock context without worker pool, execute inline */
    if (!client || !client->owner_pool || _csilk_is_owner_worker_thread(client->owner_pool)) {
        _csilk_async_op_disarm_timer(op);
        if (!client || (client->generation == op->generation && c->request_seq == op->request_seq &&
                        (c->h2_stream_owner == NULL || c->stream_gen == op->stream_gen) &&
                        !c->stream_closed)) {
            if (op->on_complete) {
                op->on_complete(c, result);
            }
        }
        _csilk_async_op_unref(op); /* Release caller's reference */
        return 0;
    }

    csilk_async_dispatch_payload_t* payload =
        (csilk_async_dispatch_payload_t*)malloc(sizeof(csilk_async_dispatch_payload_t));
    if (!payload) {
        _csilk_async_op_disarm_timer(op);
        _csilk_async_op_unref(op);
        return -1;
    }
    payload->op = op;
    payload->result = result;

    /* Increment reference for dispatch task */
    atomic_fetch_add_explicit(&op->ref_count, 1, memory_order_relaxed);
    if (_csilk_dispatch_try(op->ctx, _csilk_async_complete_dispatch_cb, payload) < 0) {
        free(payload);
        /* Release the dispatch reference acquired above. */
        _csilk_async_op_unref(op);
        return -1;
    }

    /* Release caller's reference */
    _csilk_async_op_unref(op);
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

    _csilk_async_op_disarm_timer(op);
    _csilk_async_op_unref(op); /* Release caller's reference */
    return 0;
}
