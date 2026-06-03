/**
 * @file mq_offload.c
 * @brief MQ background offload — hand off message processing to thread pool.
 *
 * Implements csilk_mq_offload(), which deep-copies the current message and
 * dispatches it to libuv's thread pool for background processing. The handler
 * chain on the main thread continues immediately without waiting.
 *
 * The worker function runs on a libuv thread pool thread with exclusive
 * ownership of the deep-copied topic and payload. Cleanup happens
 * automatically in worker_after_cb on the main loop thread.
 * @copyright MIT License
 */
#include <stdlib.h>
#include <string.h>

#include "csilk/core/mq_types.h"
#include "csilk/mq.h"

static void
worker_cb(uv_work_t* req)
{
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	wctx->handler(wctx->topic, wctx->payload, wctx->len);
}

static void
worker_after_cb(uv_work_t* req, int status)
{
	(void)status;
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	free(wctx->topic);
	free(wctx->payload);
	free(wctx);
}

void
csilk_mq_offload(csilk_mq_ctx_t* ctx, csilk_mq_worker_t worker)
{
	if (!ctx || !ctx->msg || !worker) {
		return;
	}
	csilk_mq_work_ctx_t* wctx = calloc(1, sizeof(csilk_mq_work_ctx_t));
	if (!wctx) {
		return;
	}
	wctx->req.data = wctx;
	wctx->handler = worker;
	wctx->topic = strdup(ctx->msg->topic);
	if (!wctx->topic) {
		free(wctx);
		return;
	}
	if (ctx->msg->len > 0 && ctx->msg->payload) {
		wctx->payload = malloc(ctx->msg->len);
		if (wctx->payload) {
			memcpy(wctx->payload, ctx->msg->payload, ctx->msg->len);
			wctx->len = ctx->msg->len;
		} else {
			free(wctx->topic);
			free(wctx);
			return;
		}
	}
	uv_queue_work(ctx->mq->async_handle.loop, &wctx->req, worker_cb, worker_after_cb);
	csilk_mq_next(ctx);
}
