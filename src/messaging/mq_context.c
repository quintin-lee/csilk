/**
 * @file mq_context.c
 * @brief MQ context API — chain control and message accessors.
 *
 * Provides the per-message context functions that middleware and subscriber
 * handlers use to control the execution chain and access message data:
 *   - csilk_mq_next(): advance to the next handler in the chain.
 *   - csilk_mq_abort(): short-circuit the handler chain.
 *   - csilk_mq_get_topic(): retrieve the current message topic.
 *   - csilk_mq_get_payload(): retrieve the current message payload.
 * @copyright MIT License
 */
#include "csilk/core/mq_types.h"
#include "csilk/mq.h"

void
csilk_mq_next(csilk_mq_ctx_t* ctx)
{
	if (!ctx || ctx->aborted) {
		return;
	}
	ctx->handler_index++;
	if (ctx->handler_index < (int)ctx->handler_count) {
		ctx->handlers[ctx->handler_index](ctx);
	}
}

void
csilk_mq_abort(csilk_mq_ctx_t* ctx)
{
	if (ctx) {
		ctx->aborted = 1;
	}
}

const char*
csilk_mq_get_topic(csilk_mq_ctx_t* ctx)
{
	return (ctx && ctx->msg) ? ctx->msg->topic : nullptr;
}

const void*
csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len)
{
	if (!ctx || !ctx->msg) {
		return nullptr;
	}
	if (len) {
		*len = ctx->msg->len;
	}
	return ctx->msg->payload;
}
