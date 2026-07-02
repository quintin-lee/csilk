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
#include "mq_internal.h"
#include "csilk/csilk.h"
#include "csilk/mq.h"

/** @brief Advance to the next handler in the middleware chain.
 *
 * Increments handler_index and invokes the next handler. If the chain has
 * been aborted (ctx->aborted == 1), this is a no-op — the chain stops.
 *
 * ## Typical usage inside a handler
 * @code{.c}
 * static void my_mw(csilk_mq_ctx_t* ctx) {
 *     const char* topic = csilk_mq_get_topic(ctx);
 *     if (should_process(topic)) {
 *         // ... do work ...
 *     }
 *     csilk_mq_next(ctx); // continue the chain
 * }
 * @endcode
 *
 * @param ctx The per-message context. If nullptr or aborted, does nothing. */
void
csilk_mq_next(csilk_mq_ctx_t* ctx)
{
    /* If the context is nullptr or the chain was aborted, stop */
    if (!ctx) {
        return;
    }
    if (ctx->aborted) {
        CSILK_LOG_T("MQ: Chain execution skipped: context is aborted (topic: '%s')",
                    ctx->msg ? ctx->msg->topic : "");
        return;
    }

    /* Advance to the next handler in the chain and invoke it */
    ctx->handler_index++;
    CSILK_LOG_T("MQ: Advancing to handler index %d/%zu in chain for topic '%s'",
                ctx->handler_index,
                ctx->handler_count,
                ctx->msg ? ctx->msg->topic : "");

    if (ctx->handler_index < (int)ctx->handler_count) {
        ctx->handlers[ctx->handler_index](ctx);
    }
    /* If handler_index == handler_count, the chain is complete —
     * the message processing ends naturally here. */
}

/** @brief Short-circuit the handler chain for the current message.
 *
 * Sets ctx->aborted = 1. Subsequent calls to csilk_mq_next() will be
 * no-ops, and no further handlers run. The current handler still
 * completes, but the chain stops after it returns.
 *
 * Useful for early-exit scenarios:
 *   - Authorization failure ("not authorized, abort")
 *   - Message validation failure ("malformed payload, abort")
 *   - Rate limiting ("too many requests, abort")
 *
 * @param ctx The per-message context. If nullptr, does nothing. */
void
csilk_mq_abort(csilk_mq_ctx_t* ctx)
{
    if (ctx) {
        CSILK_LOG_I("MQ: Aborting handler chain at index %d/%zu for topic '%s'",
                    ctx->handler_index,
                    ctx->handler_count,
                    ctx->msg ? ctx->msg->topic : "");
        ctx->aborted = 1;
    }
}

/** @brief Get the topic string of the current message.
 *
 * @param ctx The per-message context.
 * @return The topic string (same lifetime as the message), or nullptr if
 *         ctx is nullptr or no message has been associated. */
const char*
csilk_mq_get_topic(csilk_mq_ctx_t* ctx)
{
    return (ctx && ctx->msg) ? ctx->msg->topic : nullptr;
}

/** @brief Get the payload data and length of the current message.
 *
 * @param ctx The per-message context.
 * @param[out] len If non-nullptr, receives the payload length in bytes.
 * @return Pointer to the payload data (same lifetime as the message), or
 *         nullptr if ctx is nullptr or no message has been associated.
 * @note The returned pointer is owned by the MQ internals — do NOT free it.
 *       The payload is a deep copy, but it belongs to the message which is
 *       freed once all handlers complete. */
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
