#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "csilk/core/internal.h"
#include "mq_internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/mq.h"

/* Forward declaration for synchronous MQ dispatch in io_uring backend */
void on_mq_async(csilk_io_async_t* handle);

static void
_mq_broadcast(csilk_mq_t* mq, const char* event, const char* topic, size_t len)
{
	if (!mq || mq->monitor_count == 0) {
		return;
	}

	CSILK_LOG_T("MQ: Broadcasting monitor event '%s' for topic '%s' (len: %zu) to %zu monitors",
		    event,
		    topic ? topic : "",
		    len,
		    mq->monitor_count);

	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (topic) {
		cJSON_AddStringToObject(root, "topic", topic);
	}
	cJSON_AddNumberToObject(root, "payload_len", (double)len);
	cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));
	char* json = cJSON_PrintUnformatted(root);

	csilk_mutex_lock(&mq->monitor_mutex);
	for (size_t i = 0; i < mq->monitor_count; i++) {
		csilk_ws_send(mq->monitors[i], (uint8_t*)json, strlen(json), 0x1);
	}
	csilk_mutex_unlock(&mq->monitor_mutex);

	free(json);
	cJSON_Delete(root);
}

CSILK_INTERNAL int
_mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	CSILK_LOG_T("MQ: Enqueuing message on topic '%s' (len: %zu)", topic, len);
	csilk_mq_msg_t* msg = calloc(1, sizeof(csilk_mq_msg_t));
	if (!msg) {
		CSILK_LOG_E("MQ: Failed to allocate memory for message enqueued on topic '%s'",
			    topic);
		return -1;
	}
	msg->topic = strdup(topic);
	if (!msg->topic) {
		CSILK_LOG_E("MQ: Failed to duplicate topic name '%s' for enqueued message", topic);
		free(msg);
		return -1;
	}
	if (len > 0 && payload) {
		msg->payload = malloc(len + 1);
		if (!msg->payload) {
			CSILK_LOG_E("MQ: Failed to allocate payload memory (len: %zu) for enqueued "
				    "message on topic '%s'",
				    len,
				    topic);
			free(msg->topic);
			free(msg);
			return -1;
		}
		memcpy(msg->payload, payload, len);
		((char*)msg->payload)[len] = '\0';
		msg->len = len;
	}

	csilk_mutex_lock(&mq->queue_mutex);
	if (mq->queue_tail) {
		mq->queue_tail->next = msg;
	} else {
		mq->queue_head = msg;
	}
	mq->queue_tail = msg;

	mq->published_total++;
	mq->queue_depth++;
	csilk_mutex_unlock(&mq->queue_mutex);

	_mq_broadcast(mq, "mq_published", topic, len);
	csilk_io_async_send(&mq->async_handle);

	/* In the io_uring backend, async_send is a no-op (eventfd write).
	 * If no event loop is running, process the queue synchronously. */
#ifndef CSILK_USE_URING
	(void)0; /* libuv handles async dispatch automatically */
#else
	{
		csilk_mutex_lock(&mq->queue_mutex);
		int has_messages = (mq->queue_head != NULL);
		csilk_mutex_unlock(&mq->queue_mutex);
		if (has_messages) {
			/* Simulate async wake by processing the queue directly */
			csilk_io_async_t fake_async = {0};
			fake_async.data = mq;
			on_mq_async((csilk_io_async_t*)&fake_async);
		}
	}
#endif
	return 0;
}

int
csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	if (!mq || !topic) {
		CSILK_LOG_E("MQ: Publish failed: invalid arguments (mq: %p, topic: %p)",
			    (void*)mq,
			    (void*)topic);
		return -1;
	}

	CSILK_LOG_D("MQ: Publishing message on topic '%s' (len: %zu)", topic, len);

	if (mq->wal_fd >= 0) {
		if (_mq_append_wal(mq, topic, payload, len) != 0) {
			CSILK_LOG_E("MQ: Failed to append message to WAL for topic '%s'", topic);
			return -1;
		}
	}

	return _mq_enqueue(mq, topic, payload, len);
}

CSILK_INTERNAL void
on_mq_async(csilk_io_async_t* handle)
{
	csilk_mq_t* mq = (csilk_mq_t*)handle->data;

	csilk_mutex_lock(&mq->queue_mutex);
	csilk_mq_msg_t* head = mq->queue_head;
	mq->queue_head = nullptr;
	mq->queue_tail = nullptr;
	uint32_t count = mq->queue_depth;
	mq->queue_depth = 0;
	csilk_mutex_unlock(&mq->queue_mutex);

	CSILK_LOG_D("MQ: Async worker triggered processing %u messages from queue", count);

	while (head) {
		csilk_mq_msg_t* msg = head;
		head = head->next;

		CSILK_LOG_T("MQ: Processing message from queue. Topic: '%s', Length: %zu",
			    msg->topic,
			    msg->len);

		_mq_broadcast(mq, "mq_delivered", msg->topic, msg->len);
		csilk_mutex_lock(&mq->queue_mutex);
		mq->delivered_total++;
		csilk_mutex_unlock(&mq->queue_mutex);

		size_t total_handlers = mq->global_mw_count;
		for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
			if (fnmatch(t->name, msg->topic, 0) == 0) {
				total_handlers += t->handler_count;
			}
		}

		if (total_handlers > 0) {
			csilk_mq_handler_t* chain =
			    malloc(total_handlers * sizeof(csilk_mq_handler_t));
			if (chain) {
				size_t idx = 0;

				if (mq->global_mw_count > 0) {
					memcpy(chain,
					       mq->global_middlewares,
					       mq->global_mw_count * sizeof(csilk_mq_handler_t));
					idx += mq->global_mw_count;
				}

				for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
					if (fnmatch(t->name, msg->topic, 0) == 0 &&
					    t->handler_count > 0) {
						memcpy(chain + idx,
						       t->handlers,
						       t->handler_count *
							   sizeof(csilk_mq_handler_t));
						idx += t->handler_count;
					}
				}

				CSILK_LOG_T("MQ: Executing handler chain with %zu total handlers "
					    "for topic '%s'",
					    total_handlers,
					    msg->topic);
				csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1, 0};
				csilk_mq_next(&ctx);
				free(chain);
			} else {
				CSILK_LOG_E("MQ: Failed to allocate memory for handler chain "
					    "(total_handlers: %zu) for topic '%s'",
					    total_handlers,
					    msg->topic);
			}
		} else {
			CSILK_LOG_D("MQ: No handlers registered for topic '%s'", msg->topic);
		}

		free(msg->topic);
		free(msg->payload);
		free(msg);
	}
}
