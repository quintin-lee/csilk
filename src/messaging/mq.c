/**
 * @file mq.c
 * @brief Message Queue core — setup, publish, dispatch, and teardown.
 *
 * Implements the central MQ engine: instance creation (csilk_mq_new),
 * middleware/subscriber registration (csilk_mq_use, csilk_mq_subscribe),
 * publishing (csilk_mq_publish), async dispatch (on_mq_async), monitoring
 * (csilk_mq_get_stats, csilk_mq_register_monitor), and teardown
 * (csilk_mq_free).
 *
 * Context API moved to mq_context.c, offload to mq_offload.c, and WAL
 * persistence to mq_wal.c.
 * @copyright MIT License
 */
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/core/mq_types.h"
#include "csilk/csilk.h"
#include "csilk/mq.h"

#include "mq_internal.h"

/* --- Setup API --- */

/** @brief libuv async callback for processing queued MQ messages.
 * @param handle libuv async handle. */
static void on_mq_async(uv_async_t* handle);

/** @brief Internal: Enqueue a message into the in-memory linked list.
 *
 * Copies the topic and payload, appends to the queue's tail, and sends an
 * async signal to wake the event loop for dispatch.
 *
 * @param mq      The MQ instance.
 * @param topic   Topic string.
 * @param payload Opaque payload data.
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 on allocation failure. */
int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/** @brief Internal: Create a new Message Queue instance.
 *
 * ## Initialization
 * 1. calloc the MQ struct (zero-initialized — all counts/capacities = 0).
 * 2. Initialize the queue mutex (protects the in-memory message linked list).
 * 3. Initialize the async handle bound to the given loop. The async handle's
 *    callback (on_mq_async) is triggered by uv_async_send() whenever a new
 *    message is enqueued from any thread.
 * 4. Mark WAL as closed (wal_fd = -1), init the WAL mutex.
 *
 * The MQ starts with no topics, no handlers, and no WAL — everything is
 * populated lazily.
 *
 * @param loop libuv event loop to associate the async handle with.
 * @return A new csilk_mq_t instance, or nullptr on allocation failure.
 * @note The MQ must be freed via _csilk_mq_free(). The MQ is initially
 *       non-persistent — call csilk_mq_set_persistence() to enable WAL. */
#include <time.h>

#include "cJSON.h"

/** @brief Broadcast a monitoring event to all registered WebSocket monitors.
 *
 * Constructs a JSON event object and sends it to every monitor context.
 * Monitors receive real-time notifications of MQ activity (publish,
 * deliver, etc.) for observability and debugging.
 *
 * ## JSON event format
 * @code{.json}
 * {
 *   "event":       "mq_published" | "mq_delivered",
 *   "topic":       "user.created",
 *   "payload_len": 128,
 *   "timestamp":   1700000000
 * }
 * @endcode
 *
 * @param mq    The MQ instance.
 * @param event Event type string (e.g. "mq_published").
 * @param topic The topic of the message.
 * @param len   The payload length. */
static void
_mq_broadcast(csilk_mq_t* mq, const char* event, const char* topic, size_t len)
{
	if (!mq || mq->monitor_count == 0) {
		return;
	}

	/* Build JSON event object */
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (topic) {
		cJSON_AddStringToObject(root, "topic", topic);
	}
	cJSON_AddNumberToObject(root, "payload_len", (double)len);
	cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));
	char* json = cJSON_PrintUnformatted(root);

	/* Send to every monitor (WebSocket TEXT frame, opcode 0x1) */
	uv_mutex_lock(&mq->monitor_mutex);
	for (size_t i = 0; i < mq->monitor_count; i++) {
		csilk_ws_send(mq->monitors[i], (uint8_t*)json, strlen(json), 0x1);
	}
	uv_mutex_unlock(&mq->monitor_mutex);

	free(json);
	cJSON_Delete(root);
}

void
csilk_mq_get_stats(csilk_mq_t* mq, csilk_mq_stats_t* stats)
{
	if (!mq || !stats) {
		return;
	}
	uv_mutex_lock(&mq->queue_mutex);
	stats->published_total = mq->published_total;
	stats->delivered_total = mq->delivered_total;
	stats->failed_total = mq->failed_total;
	stats->queue_depth = mq->queue_depth;

	uint32_t topics = 0;
	for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
		topics++;
	}
	stats->topic_count = topics;
	uv_mutex_unlock(&mq->queue_mutex);
}

char*
csilk_mq_stats_to_json(const csilk_mq_stats_t* stats)
{
	if (!stats) {
		return nullptr;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "published_total", (double)stats->published_total);
	cJSON_AddNumberToObject(root, "delivered_total", (double)stats->delivered_total);
	cJSON_AddNumberToObject(root, "failed_total", (double)stats->failed_total);
	cJSON_AddNumberToObject(root, "queue_depth", (double)stats->queue_depth);
	cJSON_AddNumberToObject(root, "topic_count", (double)stats->topic_count);
	char* json = cJSON_Print(root);
	cJSON_Delete(root);
	return json;
}

void
csilk_mq_register_monitor(csilk_mq_t* mq, csilk_ctx_t* c)
{
	if (!mq || !c) {
		return;
	}
	uv_mutex_lock(&mq->monitor_mutex);
	if (mq->monitor_count >= mq->monitor_capacity) {
		size_t new_cap = mq->monitor_capacity ? mq->monitor_capacity * 2 : 4;
		mq->monitors = realloc(mq->monitors, new_cap * sizeof(csilk_ctx_t*));
		mq->monitor_capacity = new_cap;
	}
	mq->monitors[mq->monitor_count++] = c;
	uv_mutex_unlock(&mq->monitor_mutex);
}

csilk_mq_t*
_csilk_mq_new(uv_loop_t* loop)
{
	/* Allocate zero-initialized struct — counts/capacities start at 0 */
	csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
	if (!mq) {
		return nullptr;
	}

	/* Initialize mutexes for thread-safe queue/monitor operations */
	uv_mutex_init(&mq->queue_mutex);
	uv_mutex_init(&mq->monitor_mutex);

	/* Register the async handle on the event loop.
	 * uv_async_send() on this handle wakes the loop to call on_mq_async(). */
	uv_async_init(loop, &mq->async_handle, on_mq_async);
	mq->async_handle.data = mq;

	/* WAL is closed by default — csilk_mq_set_persistence() opens it */
	mq->wal_fd = -1;
	mq->wal_path = nullptr;
	uv_mutex_init(&mq->wal_mutex);

	return mq;
}

/** @brief Find or create a topic structure.
 * @param mq The MQ instance.
 * @param name Topic name.
 * @return Pointer to topic structure, or nullptr on failure. */
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
		return nullptr;
	}
	t->name = strdup(name);
	t->next = mq->topics;
	mq->topics = t;
	return t;
}

/** @brief Register a middleware handler for a specific topic (or globally).
 *
 * ## Global vs topic-specific
 * - topic == nullptr: handler is appended to mq->global_middlewares[].
 *   These run for EVERY message, regardless of topic, and execute first.
 * - topic != nullptr: handler is appended to that topic's handlers[] array.
 *   The topic is created lazily via get_or_create_topic().
 *   These run only when fnmatch(topic_name, msg_topic) matches.
 *
 * Both arrays grow by doubling (initial cap = 4) when full.
 *
 * @param mq        The MQ instance.
 * @param topic     Topic name (e.g., "user.created"), or nullptr for global.
 * @param middleware Handler function to invoke during message processing.
 * @note The handler arrays grow dynamically (doubling capacity) as needed.
 *       Topic matching supports glob patterns via fnmatch(). */
void
csilk_mq_use(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t middleware)
{
	if (!mq || !middleware) {
		return;
	}
	if (!topic) {
		if (mq->global_mw_count >= mq->global_mw_capacity) {
			size_t new_cap = mq->global_mw_capacity ? mq->global_mw_capacity * 2 : 4;
			mq->global_middlewares =
			    realloc(mq->global_middlewares, new_cap * sizeof(csilk_mq_handler_t));
			mq->global_mw_capacity = new_cap;
		}
		mq->global_middlewares[mq->global_mw_count++] = middleware;
	} else {
		csilk_mq_topic_t* t = get_or_create_topic(mq, topic);
		if (t) {
			if (t->handler_count >= t->handler_capacity) {
				size_t new_cap = t->handler_capacity ? t->handler_capacity * 2 : 4;
				t->handlers =
				    realloc(t->handlers, new_cap * sizeof(csilk_mq_handler_t));
				t->handler_capacity = new_cap;
			}
			t->handlers[t->handler_count++] = middleware;
		}
	}
}

/** @brief Register a subscriber handler for a topic.
 *
 * Subscribers are treated as handlers appended to the end of the chain
 * (after global and topic middlewares). This is a convenience wrapper
 * around csilk_mq_use().
 *
 * @param mq         The MQ instance.
 * @param topic      Topic name to subscribe to.
 * @param subscriber Handler function invoked when a message matches. */
void
csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber)
{
	/* Subscribers are essentially handlers appended to the chain */
	csilk_mq_use(mq, topic, subscriber);
}

/* --- Publishing and Async Dispatch --- */

/** @brief Internal: enqueue a message in the in-memory linked list.
 *
 * ## Enqueue algorithm
 * 1. Allocate and populate a new csilk_mq_msg_t (deep-copy topic + payload).
 * 2. Lock queue_mutex.
 * 3. Append to the tail of the singly-linked list:
 *      - If queue_tail != nullptr: queue_tail->next = msg
 *      - Else: queue_head = msg
 *    Update queue_tail to msg.
 * 4. Unlock, call uv_async_send() to wake the event loop.
 *
 * uv_async_send() is signal-safe and thread-safe — it can be called from
 * any thread. If the loop is already awake, it's a no-op (libuv coalesces).
 *
 * @param mq      The MQ instance.
 * @param topic   Message topic.
 * @param payload Message payload (may be nullptr).
 * @param len     Payload length.
 * @return 0 on success, -1 on allocation failure.
 * @note Thread-safe. The async signal ensures on_mq_async() processes the
 *       message on the main loop thread. */
int
_mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	csilk_mq_msg_t* msg = calloc(1, sizeof(csilk_mq_msg_t));
	if (!msg) {
		return -1;
	}
	msg->topic = strdup(topic);
	if (!msg->topic) {
		free(msg);
		return -1;
	}
	if (len > 0 && payload) {
		msg->payload = malloc(len + 1);
		if (!msg->payload) {
			free(msg->topic);
			free(msg);
			return -1;
		}
		memcpy(msg->payload, payload, len);
		((char*)msg->payload)[len] = '\0';
		msg->len = len;
	}

	uv_mutex_lock(&mq->queue_mutex);
	if (mq->queue_tail) {
		mq->queue_tail->next = msg;
	} else {
		mq->queue_head = msg;
	}
	mq->queue_tail = msg;

	mq->published_total++;
	mq->queue_depth++;
	uv_mutex_unlock(&mq->queue_mutex);

	_mq_broadcast(mq, "mq_published", topic, len);
	uv_async_send(&mq->async_handle);
	return 0;
}

/** @brief Publish a message to a topic (thread-safe, optionally persistent).
 *
 * ## Write-ahead: WAL then memory
 * The two operations are NOT atomic (WAL can succeed, memory enqueue can
 * fail). This is a deliberate trade-off:
 *   - If WAL succeeds but enqueue fails: message is safe on disk but lost
 *     in memory — it will be recovered on next restart.
 *   - If WAL fails: the message is dropped entirely (return -1).
 *
 * Processing is asynchronous — the caller receives no delivery confirmation.
 * The message is deep-copied at both stages.
 *
 * @param mq      The MQ instance.
 * @param topic   Target topic name (cannot be nullptr).
 * @param payload Message payload data (may be nullptr).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 if the topic is nullptr or WAL append fails.
 * @note Thread-safe. The caller may free or reuse @p payload immediately
 *       after this call returns — the data is copied internally. */
int
csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	if (!mq || !topic) {
		return -1;
	}

	/* 1. WAL: persist to disk first (if WAL is enabled).
	 *    If the WAL write fails, we return -1 without enqueuing —
	 *    the message is dropped entirely. This is stricter than the
	 *    reverse ordering (enqueue then WAL) which could lose a
	 *    delivered-but-unpersisted message on crash. */
	if (mq->wal_fd >= 0) {
		if (_mq_append_wal(mq, topic, payload, len) != 0) {
			return -1;
		}
	}

	/* 2. Memory: enqueue for async delivery on the event loop.
	 *    This always succeeds except on OOM. */
	return _mq_enqueue(mq, topic, payload, len);
}

/** @brief libuv async callback — dequeue and process all pending messages.
 *
 * ## Dispatch algorithm (per message)
 * This is the core of the MQ — it runs on the main event loop thread.
 *
 *   1. Atomically swap the queue head to nullptr under mutex (drain all).
 *   2. Walk the linked list of messages.
 *   3. For each message:
 *      a. Count total handlers: global_mw_count + sum of handler_count for
 *         every topic where fnmatch(topic->name, msg->topic) == 0.
 *      b. Allocate a contiguous handler chain array of that size.
 *      c. Bulk-copy global middlewares into the chain.
 *      d. For each matching topic: bulk-copy its handlers.
 *      e. Create a csilk_mq_ctx_t with {mq, msg, chain, count, -1, 0}.
 *      f. Kick off the chain with csilk_mq_next(&ctx) — handler at
 *         index 0 runs, which typically calls csilk_mq_next() again.
 *      g. Free the chain array (the context is stack-allocated).
 *   4. Free the message (topic, payload, struct).
 *
 * ## Topic matching via fnmatch
 * fnmatch(3) implements POSIX shell glob patterns: '*' matches any
 * sequence, '?' matches any single char, '[abc]' matches character sets.
 * This allows patterns like "user.*" or "system.event.?".
 *
 * @param handle libuv async handle (data points to csilk_mq_t). */
static void
on_mq_async(uv_async_t* handle)
{
	csilk_mq_t* mq = (csilk_mq_t*)handle->data;

	uv_mutex_lock(&mq->queue_mutex);
	csilk_mq_msg_t* head = mq->queue_head;
	mq->queue_head = nullptr;
	mq->queue_tail = nullptr;
	uint32_t count = mq->queue_depth;
	mq->queue_depth = 0;
	uv_mutex_unlock(&mq->queue_mutex);

	while (head) {
		csilk_mq_msg_t* msg = head;
		head = head->next;

		_mq_broadcast(mq, "mq_delivered", msg->topic, msg->len);
		uv_mutex_lock(&mq->queue_mutex);
		mq->delivered_total++;
		uv_mutex_unlock(&mq->queue_mutex);

		/* Count total matching handlers:
		 * global middlewares always run; topic handlers only if fnmatch matches */
		size_t total_handlers = mq->global_mw_count;
		for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
			if (fnmatch(t->name, msg->topic, 0) == 0) {
				total_handlers += t->handler_count;
			}
		}

		if (total_handlers > 0) {
			/* Allocate a contiguous handler chain array */
			csilk_mq_handler_t* chain =
			    malloc(total_handlers * sizeof(csilk_mq_handler_t));
			if (chain) {
				size_t idx = 0;

				/* Bulk-copy global middlewares (run first, in order) */
				if (mq->global_mw_count > 0) {
					memcpy(chain,
					       mq->global_middlewares,
					       mq->global_mw_count * sizeof(csilk_mq_handler_t));
					idx += mq->global_mw_count;
				}

				/* Bulk-copy matching topic handlers (appended after globals) */
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

				/* Create a stack-allocated context and kick off the chain */
				csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1, 0};
				csilk_mq_next(&ctx);
				free(chain);
			}
		}

		/* Free the message (deep copies are consumed by handlers or freed) */
		free(msg->topic);
		free(msg->payload);
		free(msg);
	}
}

/** @brief libuv close callback — final cleanup when the MQ async handle is
 * closed.
 *
 * ## Teardown order
 *   1. Destroy mutexes (queue_mutex, wal_mutex) — no more concurrent access.
 *   2. Close WAL file (uv_fs_close), free wal_path.
 *   3. Drain and free the in-memory message linked list.
 *   4. Free all topic structures: topic name, handler arrays, structs.
 *   5. Free global middlewares array.
 *   6. Free the MQ struct itself.
 *
 * This is guaranteed to run on the libuv event loop thread (uv_close
 * callback), so it is safe to destroy mutexes here.
 *
 * @param handle libuv handle being closed (data points to csilk_mq_t). */
static void
on_mq_close(uv_handle_t* handle)
{
	csilk_mq_t* mq = (csilk_mq_t*)handle->data;
	if (!mq) {
		return;
	}

	uv_mutex_destroy(&mq->queue_mutex);
	uv_mutex_destroy(&mq->wal_mutex);

	if (mq->wal_fd >= 0) {
		uv_fs_t close_req;
		uv_fs_close(handle->loop, &close_req, mq->wal_fd, nullptr);
		uv_fs_req_cleanup(&close_req);
	}
	if (mq->wal_path) {
		free(mq->wal_path);
	}

	/* Free queue */
	csilk_mq_msg_t* msg = mq->queue_head;
	while (msg) {
		csilk_mq_msg_t* next = msg->next;
		free(msg->topic);
		free(msg->payload);
		free(msg);
		msg = next;
	}

	/* Free topics */
	csilk_mq_topic_t* topic = mq->topics;
	while (topic) {
		csilk_mq_topic_t* next = topic->next;
		free(topic->name);
		free(topic->handlers);
		free(topic);
		topic = next;
	}

	/* Free global middlewares */
	free(mq->global_middlewares);

	free(mq);
}

/** @brief Internal: initiate asynchronous shutdown and free of a Message Queue.
 *
 * Triggers uv_close() on the async handle if it is not already closing.
 * The actual cleanup (mutexes, WAL, queue, topics) happens in on_mq_close()
 * when the close callback fires.
 *
 * @param mq The MQ instance to free (may be nullptr).
 * @note This is an async operation — the MQ is not freed immediately.
 *       Safe to call with nullptr. */
void
_csilk_mq_free(csilk_mq_t* mq)
{
	if (!mq) {
		return;
	}
	if (!uv_is_closing((uv_handle_t*)&mq->async_handle)) {
		mq->async_handle.data = mq;
		uv_close((uv_handle_t*)&mq->async_handle, on_mq_close);
	}
}
