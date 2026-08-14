/**
 * @file mq_pubsub.c
 * @brief MQ pub/sub registration — middleware, global middleware, subscribers.
 *
 * Implements topic handler registration.  A NULL topic registers a global
 * middleware that runs for every message; a named topic lazily creates a topic
 * node and appends the handler.  csilk_mq_subscribe is a thin alias over
 * csilk_mq_use for readability.
 * @copyright MIT License
 */

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "mq_internal.h"
#include "csilk/messaging/mq.h"
#include "mq_internal.h"

/* Returns the topic node with the given name, creating it (prepended to the
 * topic linked list) if it does not yet exist.  Returns NULL on allocation
 * failure. */
static csilk_mq_topic_t*
get_or_create_topic(csilk_mq_t* mq, const char* name)
{
    for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    csilk_mq_topic_t* t = calloc(1, sizeof(csilk_mq_topic_t));
    if (!t) {
        CSILK_LOG_E("MQ: Failed to allocate memory for topic '%s'", name);
        return NULL;
    }
    t->name = strdup(name);
    t->next = mq->topics;
    mq->topics = t;
    CSILK_LOG_D("MQ: Created topic entry '%s'", name);
    return t;
}

/**
 * @brief Register a middleware or subscriber handler for a topic.
 *
 * With @p topic == NULL the handler is appended to the global middleware array
 * (runs for all messages; array grows from capacity 0, doubling).  With a named
 * topic the handler is appended to that topic's handler chain, creating the
 * topic node on first use.  Handlers run in registration order: global
 * middleware, then matching-topic middleware, then subscribers.
 *
 * @param[in] mq         The MQ instance (must not be NULL).
 * @param[in] topic      Topic name, or NULL for global middleware.
 * @param[in] middleware Handler function (must not be NULL).
 * @note NULL arguments or allocation failure are logged and ignored.
 */
void
csilk_mq_use(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t middleware)
{
    if (!mq || !middleware) {
        CSILK_LOG_E("MQ: Failed to register handler: invalid arguments");
        return;
    }
    if (!topic) {
        if (mq->global_mw_count >= mq->global_mw_capacity) {
            size_t new_cap;
            if (mq->global_mw_capacity == 0) {
                new_cap = 4;
            } else {
                if (mq->global_mw_capacity > SIZE_MAX / 2) {
                    return;
                }
                new_cap = mq->global_mw_capacity * 2;
            }
            if (new_cap > SIZE_MAX / sizeof(csilk_mq_handler_t)) {
                return;
            }
            csilk_mq_handler_t* new_mw =
                realloc(mq->global_middlewares, new_cap * sizeof(csilk_mq_handler_t));
            if (!new_mw) {
                CSILK_LOG_E("MQ: Failed to allocate memory for global middleware");
                return;
            }
            mq->global_middlewares = new_mw;
            mq->global_mw_capacity = new_cap;
        }
        mq->global_middlewares[mq->global_mw_count++] = middleware;
        CSILK_LOG_I("MQ: Registered global middleware %p", (void*)middleware);
    } else {
        csilk_mq_topic_t* t = get_or_create_topic(mq, topic);
        if (t) {
            if (t->handler_count >= t->handler_capacity) {
                size_t new_cap;
                if (t->handler_capacity == 0) {
                    new_cap = 4;
                } else {
                    if (t->handler_capacity > SIZE_MAX / 2) {
                        return;
                    }
                    new_cap = t->handler_capacity * 2;
                }
                if (new_cap > SIZE_MAX / sizeof(csilk_mq_handler_t)) {
                    return;
                }
                csilk_mq_handler_t* new_h =
                    realloc(t->handlers, new_cap * sizeof(csilk_mq_handler_t));
                if (!new_h) {
                    CSILK_LOG_E("MQ: Failed to allocate memory for handler on "
                                "topic '%s'",
                                topic);
                    return;
                }
                t->handlers = new_h;
                t->handler_capacity = new_cap;
            }
            t->handlers[t->handler_count++] = middleware;
            CSILK_LOG_I("MQ: Registered subscription/middleware %p for topic '%s'",
                        (void*)middleware,
                        topic);
        }
    }
}

/**
 * @brief Register a subscriber handler for a named topic.
 *
 * Alias for csilk_mq_use(mq, topic, subscriber); subscribers run after all
 * applicable middleware for the topic.
 *
 * @param[in] mq         The MQ instance.
 * @param[in] topic      Topic name to subscribe to (must not be NULL).
 * @param[in] subscriber Handler function (must not be NULL).
 */
void
csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber)
{
    csilk_mq_use(mq, topic, subscriber);
}
