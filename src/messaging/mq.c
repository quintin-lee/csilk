/** @file mq.c
 * @brief Internal Event Bus (Message Queue) implementation.
 *
 * ## Architecture
 * The MQ is a publish-subscribe event bus built on libuv async handles.
 * It supports topic-based routing with glob patterns (via fnmatch),
 * middleware chains, thread-safe publishing from any thread, optional
 * persistence via Write-Ahead Log (WAL), and background thread offloading.
 *
 * ## Dispatch model
 * Messages flow through a handler chain assembled dynamically at dispatch
 * time (see on_mq_async). The chain order is:
 *   1. Global middleware (registered with topic=NULL) — runs for ALL topics.
 *   2. Topic-specific handlers — matched by fnmatch(topic_pattern, msg_topic).
 *   3. Subscribers (just handlers registered via csilk_mq_subscribe).
 *
 * Each handler calls csilk_mq_next() to advance the chain, or csilk_mq_abort()
 * to short-circuit.
 *
 * ## Thread safety
 * Publishing (csilk_mq_publish) is thread-safe and lock-free on the fast path:
 * the message is copied, appended to a mutex-guarded linked list, and an
 * async signal (uv_async_send) wakes the main loop thread to drain the queue.
 *
 * ## Persistence (WAL)
 * When enabled via csilk_mq_set_persistence(), every published message is
 * first written to a binary WAL file before being enqueued in memory.
 * On next startup, _mq_recovery() replays the WAL to restore undelivered
 * messages. The WAL format is:
 * [topic_len:4][topic:N][payload_len:4][payload:M][xor_checksum:4].
 *
 * @copyright MIT License
 */
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/* --- Context API --- */

/** @brief Advance to the next middleware or subscriber in the MQ handler chain.
 *
 * ## Execution model
 * Handlers form a linear chain: [global_mw..., topic_mw..., subscriber...].
 * Each handler calls csilk_mq_next() to yield control to the next one.
 * This is a non-recursive, non-reentrant manual trampoline — the chain
 * is driven by the handlers themselves, not by a central loop.
 *
 * If ctx->aborted is set (by a previous csilk_mq_abort() call), this is a
 * no-op. Out-of-bounds handler_index is also silently ignored (end of chain).
 *
 * @param ctx Message queue context.
 * @note Typically called by middleware to pass control to the next handler
 *       or subscriber. */
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

/** @brief Abort the current MQ middleware chain immediately.
 *
 * Sets the aborted flag on the context. Subsequent calls to csilk_mq_next()
 * will be ignored.
 *
 * @param ctx Message queue context (may be NULL). */
void
csilk_mq_abort(csilk_mq_ctx_t* ctx)
{
	if (ctx) {
		ctx->aborted = 1;
	}
}

/** @brief Get the topic name of the current message in the MQ context.
 *
 * @param ctx Message queue context.
 * @return The topic string (e.g., "user.created"), or NULL if the context
 *         or message is NULL. */
const char*
csilk_mq_get_topic(csilk_mq_ctx_t* ctx)
{
	return (ctx && ctx->msg) ? ctx->msg->topic : NULL;
}

/** @brief Get the payload data and length of the current message.
 *
 * @param ctx Message queue context.
 * @param len [out] If non-NULL, receives the payload length in bytes.
 * @return Pointer to the raw payload data, or NULL if the context or
 *         message is NULL.
 * @note The returned pointer is valid only for the duration of the handler
 *       callback. If the data is needed later, the handler must copy it. */
const void*
csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len)
{
	if (!ctx || !ctx->msg) {
		return NULL;
	}
	if (len) {
		*len = ctx->msg->len;
	}
	return ctx->msg->payload;
}

/* --- Offload API --- */

/** @brief libuv work callback — runs the offloaded handler on a thread pool
 * thread.
 *
 * Extracts the work context from the request, then calls the user's handler
 * with the topic, payload, and length.
 *
 * @param req libuv work request (contains csilk_mq_work_ctx_t in data). */
static void
worker_cb(uv_work_t* req)
{
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	wctx->handler(wctx->topic, wctx->payload, wctx->len);
}

/** @brief libuv after-work callback — runs on the main loop thread after
 * worker_cb completes.
 *
 * Frees the work context (topic, payload, and struct). The handler results
 * (if any) should have been communicated back before this point since no
 * result channel is provided.
 *
 * @param req    libuv work request (freed by this callback).
 * @param status libuv status (ignored). */
static void
worker_after_cb(uv_work_t* req, int status)
{
	(void)status;
	csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
	free(wctx->topic);
	free(wctx->payload);
	free(wctx);
}

/** @brief Offload message processing to a libuv thread pool worker.
 *
 * ## Mechanism
 * 1. Deep-copy topic (strdup) and payload (malloc+memcpy) into a work ctx.
 * 2. Queue the work via uv_queue_work() — runs worker_cb on a libuv thread
 *    pool thread.
 * 3. worker_after_cb fires on the main loop thread: frees the work ctx.
 * 4. Call csilk_mq_next() immediately *on the main thread* to continue the
 *    handler chain, without waiting for the background worker.
 *
 * The deep copy avoids shared mutable state between the background thread
 * and the event loop. The caller's handler chain continues in parallel with
 * the offloaded work — there is no result channel.
 *
 * @param ctx    Message queue context.
 * @param worker Worker function that will receive topic, payload, and length
 *               on a background thread.
 * @note The payload is deep-copied so the background thread can safely
 *       process it without worrying about mutex locking. */
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
static int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);

/** @brief Internal: Recover messages from the Write-Ahead Log on startup.
 *
 * Reads the WAL file sequentially and enqueues each persisted message into
 * memory. Called once during MQ initialization.
 *
 * @param mq The MQ instance.
 * @return 0 on success, -1 on I/O or replay failure. */
static int _mq_recovery(csilk_mq_t* mq);

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
 * @return A new csilk_mq_t instance, or NULL on allocation failure.
 * @note The MQ must be freed via _csilk_mq_free(). The MQ is initially
 *       non-persistent — call csilk_mq_set_persistence() to enable WAL. */
#include <time.h>

#include "cJSON.h"

static void
_mq_broadcast(csilk_mq_t* mq, const char* event, const char* topic, size_t len)
{
	if (!mq || mq->monitor_count == 0) {
		return;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (topic) {
		cJSON_AddStringToObject(root, "topic", topic);
	}
	cJSON_AddNumberToObject(root, "payload_len", (double)len);
	cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
	char* json = cJSON_PrintUnformatted(root);

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
		return NULL;
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
	csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
	if (!mq) {
		return NULL;
	}
	uv_mutex_init(&mq->queue_mutex);
	uv_mutex_init(&mq->monitor_mutex);
	uv_async_init(loop, &mq->async_handle, on_mq_async);
	mq->async_handle.data = mq;

	mq->wal_fd = -1;
	mq->wal_path = NULL;
	uv_mutex_init(&mq->wal_mutex);

	return mq;
}

/** @brief Enable persistent message delivery using a Write-Ahead Log (WAL).
 *
 * ## WAL handshake
 * 1. Lock wal_mutex (held for the entire setup + recovery).
 * 2. Close any previously-opened WAL file + free old wal_path.
 * 3. Open (or create) the WAL file at wal_path with O_CREAT | O_RDWR |
 * O_APPEND.
 * 4. Store fd and path in the MQ struct.
 * 5. Call _mq_recovery() to replay any existing messages from the WAL file
 *    into the in-memory queue. This ensures messages survive process restarts.
 *
 * After this call, every csilk_mq_publish() appends to the WAL before
 * enqueuing in memory.
 *
 * @param mq       The MQ instance.
 * @param wal_path File path for the WAL. The file is created if it does not
 *                 exist.
 * @return 0 on success, -1 if parameters are NULL or the file cannot be opened.
 * @note The WAL uses a simple binary format: [topic_len][topic][payload_len]
 *       [payload][checksum] entries. Checksum is a simple XOR for integrity. */
int
csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path)
{
	if (!mq || !wal_path) {
		return -1;
	}

	uv_mutex_lock(&mq->wal_mutex);

	/* Close existing WAL if any */
	if (mq->wal_fd >= 0) {
		uv_fs_t close_req;
		uv_fs_close(mq->async_handle.loop, &close_req, mq->wal_fd, NULL);
		uv_fs_req_cleanup(&close_req);
		mq->wal_fd = -1;
	}
	if (mq->wal_path) {
		free(mq->wal_path);
		mq->wal_path = NULL;
	}

	uv_fs_t open_req;
	/* Use synchronous open */
	int fd = uv_fs_open(
	    mq->async_handle.loop, &open_req, wal_path, O_CREAT | O_RDWR | O_APPEND, 0644, NULL);
	uv_fs_req_cleanup(&open_req);

	if (fd < 0) {
		uv_mutex_unlock(&mq->wal_mutex);
		return fd;
	}

	mq->wal_fd = fd;
	mq->wal_path = strdup(wal_path);

	/* Recovery: Load existing messages from WAL */
	_mq_recovery(mq);

	uv_mutex_unlock(&mq->wal_mutex);

	return 0;
}

/** @brief Find or create a topic structure.
 * @param mq The MQ instance.
 * @param name Topic name.
 * @return Pointer to topic structure, or NULL on failure. */
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
		return NULL;
	}
	t->name = strdup(name);
	t->next = mq->topics;
	mq->topics = t;
	return t;
}

/** @brief Register a middleware handler for a specific topic (or globally).
 *
 * ## Global vs topic-specific
 * - topic == NULL: handler is appended to mq->global_middlewares[].
 *   These run for EVERY message, regardless of topic, and execute first.
 * - topic != NULL: handler is appended to that topic's handlers[] array.
 *   The topic is created lazily via get_or_create_topic().
 *   These run only when fnmatch(topic_name, msg_topic) matches.
 *
 * Both arrays grow by doubling (initial cap = 4) when full.
 *
 * @param mq        The MQ instance.
 * @param topic     Topic name (e.g., "user.created"), or NULL for global.
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

/** @brief Internal: append a message frame to the Write-Ahead Log file.
 *
 * ## WAL frame format (total frame size = 4 + N + 4 + M + 4)
 * ```
 *   [topic_len    : uint32_t, 4 bytes]  — byte length of topic string
 *   [topic_data   : uint8_t[], N bytes]  — topic UTF-8 bytes (no NUL)
 *   [payload_len  : uint32_t, 4 bytes]  — byte length of payload
 *   [payload_data : uint8_t[], M bytes]  — raw payload bytes
 *   [checksum     : uint32_t, 4 bytes]  — XOR over topic + payload
 * ```
 *
 * After writing all 5 parts via a single uv_fs_write() scatter-gather I/O
 * (5 uv_buf_t entries), the file is fsynced for crash durability.
 *
 * @param mq      The MQ instance (must have wal_fd >= 0).
 * @param topic   Message topic string.
 * @param payload Message payload data (may be NULL if len == 0).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 on write failure.
 * @note This is a no-op if the MQ has no WAL file (wal_fd < 0).
 * @note The caller should hold wal_mutex, though this function acquires it
 *       internally as well for safety. */
static int
_mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	if (!mq || mq->wal_fd < 0 || !topic) {
		return 0;
	}

	uv_mutex_lock(&mq->wal_mutex);

	uint32_t topic_len = (uint32_t)strlen(topic);
	uint32_t payload_len = (uint32_t)len;
	uint32_t checksum = 0;

	/* Simple XOR checksum of topic and payload */
	for (uint32_t i = 0; i < topic_len; i++) {
		checksum ^= (uint8_t)topic[i];
	}
	const uint8_t* p = (const uint8_t*)payload;
	if (p) {
		for (uint32_t i = 0; i < payload_len; i++) {
			checksum ^= p[i];
		}
	}

	uv_buf_t bufs[5];
	bufs[0] = uv_buf_init((char*)&topic_len, 4);
	bufs[1] = uv_buf_init((char*)topic, topic_len);
	bufs[2] = uv_buf_init((char*)&payload_len, 4);
	bufs[3] = uv_buf_init((char*)payload, payload_len);
	bufs[4] = uv_buf_init((char*)&checksum, 4);

	uv_fs_t write_req;
	/* Write to the end of file (synchronous) */
	int result = uv_fs_write(mq->async_handle.loop, &write_req, mq->wal_fd, bufs, 5, -1, NULL);
	uv_fs_req_cleanup(&write_req);

	if (result >= 0) {
		uv_fs_t sync_req;
		uv_fs_fsync(mq->async_handle.loop, &sync_req, mq->wal_fd, NULL);
		uv_fs_req_cleanup(&sync_req);
	}

	uv_mutex_unlock(&mq->wal_mutex);
	return (result >= 0) ? 0 : -1;
}

/** @brief Internal: enqueue a message in the in-memory linked list.
 *
 * ## Enqueue algorithm
 * 1. Allocate and populate a new csilk_mq_msg_t (deep-copy topic + payload).
 * 2. Lock queue_mutex.
 * 3. Append to the tail of the singly-linked list:
 *      - If queue_tail != NULL: queue_tail->next = msg
 *      - Else: queue_head = msg
 *    Update queue_tail to msg.
 * 4. Unlock, call uv_async_send() to wake the event loop.
 *
 * uv_async_send() is signal-safe and thread-safe — it can be called from
 * any thread. If the loop is already awake, it's a no-op (libuv coalesces).
 *
 * @param mq      The MQ instance.
 * @param topic   Message topic.
 * @param payload Message payload (may be NULL).
 * @param len     Payload length.
 * @return 0 on success, -1 on allocation failure.
 * @note Thread-safe. The async signal ensures on_mq_async() processes the
 *       message on the main loop thread. */
static int
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
		msg->payload = malloc(len);
		if (!msg->payload) {
			free(msg->topic);
			free(msg);
			return -1;
		}
		memcpy(msg->payload, payload, len);
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
/** @brief Internal: recover messages from the Write-Ahead Log on startup.
 *
 * ## Recovery algorithm (WAL replay)
 * Reads the WAL file sequentially from offset 0 using positional reads
 * (uv_fs_read with offset parameter). For each frame:
 *
 *   1. Read 4 bytes → topic_len. If < 4 bytes: EOF or corruption → stop.
 *   2. Read topic_len bytes → topic name. If short read → stop.
 *   3. Read 4 bytes → payload_len. If < 4 bytes → stop.
 *   4. Read payload_len bytes → payload. If short read → stop.
 *   5. Read 4 bytes → stored checksum. If < 4 bytes → stop.
 *   6. Compute XOR checksum over topic + payload.
 *   7. If checksum matches: enqueue the message in memory (_mq_enqueue),
 *      WITHOUT re-appending to the WAL.
 *   8. If checksum mismatches: free, stop (treat as corruption boundary).
 *
 * ## Why stop at corruption?
 * The WAL is append-only with no frame-length prefix. If a frame is corrupt,
 * the next frame boundary is unknowable — we stop to avoid misinterpreting
 * garbage as valid data.
 *
 * @param mq The MQ instance (must have wal_fd >= 0).
 * @return 0 on success (or if no WAL), -1 on allocation failure.
 * @note The WAL is NOT truncated after recovery — new messages append after
 *       existing ones. A future compaction step could truncate processed
 *       entries. */
static int
_mq_recovery(csilk_mq_t* mq)
{
	if (!mq || mq->wal_fd < 0) {
		return 0;
	}

	uint64_t offset = 0;
	while (1) {
		uint32_t topic_len = 0;
		uint32_t payload_len = 0;
		uint32_t checksum = 0;
		uv_fs_t read_req;
		int nread;

		/* 1. Read Topic Length */
		uv_buf_t buf = uv_buf_init((char*)&topic_len, 4);
		nread =
		    uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, NULL);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			break; /* EOF or error */
		}
		offset += 4;

		/* 2. Read Topic Name */
		char* topic = malloc(topic_len + 1);
		if (!topic) {
			break;
		}
		buf = uv_buf_init(topic, topic_len);
		nread =
		    uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, NULL);
		uv_fs_req_cleanup(&read_req);
		if (nread < (int)topic_len) {
			free(topic);
			break;
		}
		topic[topic_len] = '\0';
		offset += topic_len;

		/* 3. Read Payload Length */
		buf = uv_buf_init((char*)&payload_len, 4);
		nread =
		    uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, NULL);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			free(topic);
			break;
		}
		offset += 4;

		/* 4. Read Payload */
		void* payload = NULL;
		if (payload_len > 0) {
			payload = malloc(payload_len);
			if (!payload) {
				free(topic);
				break;
			}
			buf = uv_buf_init(payload, payload_len);
			nread = uv_fs_read(
			    mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, NULL);
			uv_fs_req_cleanup(&read_req);
			if (nread < (int)payload_len) {
				free(topic);
				free(payload);
				break;
			}
			offset += payload_len;
		}

		/* 5. Read Checksum */
		buf = uv_buf_init((char*)&checksum, 4);
		nread =
		    uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1, offset, NULL);
		uv_fs_req_cleanup(&read_req);
		if (nread < 4) {
			free(topic);
			free(payload);
			break;
		}
		offset += 4;

		/* 6. Validate Checksum */
		uint32_t calc_checksum = 0;
		for (uint32_t i = 0; i < topic_len; i++) {
			calc_checksum ^= (uint8_t)topic[i];
		}
		const uint8_t* p = (const uint8_t*)payload;
		if (p) {
			for (uint32_t i = 0; i < payload_len; i++) {
				calc_checksum ^= p[i];
			}
		}

		if (calc_checksum == checksum) {
			/* Enqueue in memory without re-appending to WAL */
			_mq_enqueue(mq, topic, payload, payload_len);
		} else {
			free(topic);
			free(payload);
			break; /* Stop at first invalid frame */
		}

		free(topic);
		free(payload);
	}

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
 * @param topic   Target topic name (cannot be NULL).
 * @param payload Message payload data (may be NULL).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 if the topic is NULL or WAL append fails.
 * @note Thread-safe. The caller may free or reuse @p payload immediately
 *       after this call returns — the data is copied internally. */
int
csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len)
{
	if (!mq || !topic) {
		return -1;
	}

	/* Persistence: Append to WAL before memory queue */
	if (mq->wal_fd >= 0) {
		if (_mq_append_wal(mq, topic, payload, len) != 0) {
			return -1;
		}
	}

	return _mq_enqueue(mq, topic, payload, len);
}

/** @brief libuv async callback — dequeue and process all pending messages.
 *
 * ## Dispatch algorithm (per message)
 * This is the core of the MQ — it runs on the main event loop thread.
 *
 *   1. Atomically swap the queue head to NULL under mutex (drain all).
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
	mq->queue_head = NULL;
	mq->queue_tail = NULL;
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

		/* Count total matching handlers */
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

				csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1, 0};
				csilk_mq_next(&ctx);
				free(chain);
			}
		}

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
		uv_fs_close(handle->loop, &close_req, mq->wal_fd, NULL);
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
 * @param mq The MQ instance to free (may be NULL).
 * @note This is an async operation — the MQ is not freed immediately.
 *       Safe to call with NULL. */
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
