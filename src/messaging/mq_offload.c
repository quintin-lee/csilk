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

/** @brief libuv work callback — runs on a thread pool thread.
 *
 * Extracts the work context from the uv_work_t and invokes the user's
 * worker callback with the deep-copied topic and payload. This runs
 * outside the event loop thread — do NOT call any csilk_mq_* functions
 * or libuv handle operations inside the handler.
 *
 * @param req libuv work request (data points to csilk_mq_work_ctx_t). */
static void
worker_cb(uv_work_t* req)
{
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	wctx->handler(wctx->topic, wctx->payload, wctx->len);
}

/** @brief libuv after-work callback — runs on the main loop thread.
 *
 * Frees the deep-copied topic, payload, and the work context struct
 * itself. Runs after worker_cb() completes, on the main event loop
 * thread, so it is safe to call standard csilk APIs here if needed.
 *
 * @param req    libuv work request (data points to csilk_mq_work_ctx_t).
 * @param status 0 on success, or a negative error code (unused here). */
static void
worker_after_cb(uv_work_t* req, int status)
{
	(void)status;
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	free(wctx->topic);
	free(wctx->payload);
	free(wctx);
}

/** @brief Offload message processing to libuv's thread pool (fire-and-forget).
 *
 * Deep-copies the topic and payload from the current message context into
 * a newly allocated csilk_mq_work_ctx_t, then dispatches it via
 * uv_queue_work(). The provided worker callback runs on a thread pool
 * thread, and the main thread handler chain continues immediately via
 * csilk_mq_next().
 *
 * ## Lifecycle
 *   - The caller's handler on the main thread calls csilk_mq_offload()
 *     and then returns — csilk_mq_next() ensures the chain advances.
 *   - uv_queue_work() owns the wctx until worker_after_cb() runs.
 *   - worker_after_cb() frees the deep copies and the wctx struct.
 *
 * ## Usage example
 * @code{.c}
 * static void on_message(csilk_mq_ctx_t* ctx) {
 *     csilk_mq_offload(ctx, process_in_background);
 * }
 * static void process_in_background(const char* topic,
 *                                   const void* payload, size_t len) {
 *     // Runs on thread pool — heavy I/O or CPU work here
 * }
 * @endcode
 *
 * @param ctx    The per-message context (must have a valid msg).
 * @param worker The callback to invoke on the thread pool thread.
 * @note If ctx, ctx->msg, or worker is NULL, this is a no-op.
 * @note The worker callback owns the topic and payload pointers for its
 *       duration — they are freed automatically after it returns. */
void
csilk_mq_offload(csilk_mq_ctx_t* ctx, csilk_mq_worker_t worker)
{
	if (!ctx || !ctx->msg || !worker) {
		return;
	}

	/* Allocate the work context that lives across the thread pool dispatch */
	csilk_mq_work_ctx_t* wctx = calloc(1, sizeof(csilk_mq_work_ctx_t));
	if (!wctx) {
		return;
	}
	wctx->req.data = wctx;
	wctx->handler = worker;

	/* Deep-copy topic — the message struct is freed after dispatch */
	wctx->topic = strdup(ctx->msg->topic);
	if (!wctx->topic) {
		free(wctx);
		return;
	}

	/* Deep-copy payload (if present) */
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

	/* Dispatch to libuv thread pool.
	 * worker_cb() runs on a thread pool thread.
	 * worker_after_cb() runs on the main loop thread. */
	uv_queue_work(ctx->mq->async_handle.loop, &wctx->req, worker_cb, worker_after_cb);

	/* Continue the handler chain on the main thread */
	csilk_mq_next(ctx);
}
