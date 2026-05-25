/** @file mq.c
 * @brief Internal Event Bus (Message Queue) implementation.
 * @copyright MIT License
 */
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_internal.h"

/* --- Context API --- */

/** @brief Advance to the next middleware or subscriber in the MQ handler chain.
 *
 * Increments the handler index and calls the next handler in the chain.
 * If the chain has been aborted (via csilk_mq_abort()) or there are no more
 * handlers, this is a no-op.
 *
 * @param ctx Message queue context.
 * @note Typically called by middleware to pass control to the next handler
 *       or subscriber. */
void csilk_mq_next(csilk_mq_ctx_t* ctx) {
  if (!ctx || ctx->aborted) return;
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
void csilk_mq_abort(csilk_mq_ctx_t* ctx) {
  if (ctx) ctx->aborted = 1;
}

/** @brief Get the topic name of the current message in the MQ context.
 *
 * @param ctx Message queue context.
 * @return The topic string (e.g., "user.created"), or NULL if the context
 *         or message is NULL. */
const char* csilk_mq_get_topic(csilk_mq_ctx_t* ctx) {
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
const void* csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len) {
  if (!ctx || !ctx->msg) return NULL;
  if (len) *len = ctx->msg->len;
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
static void worker_cb(uv_work_t* req) {
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
static void worker_after_cb(uv_work_t* req, int status) {
  (void)status;
  csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
  free(wctx->topic);
  free(wctx->payload);
  free(wctx);
}

/** @brief Offload message processing to a libuv thread pool worker.
 *
 * Creates a work context containing copies of the topic and payload, then
 * queues the work via uv_queue_work(). After the worker completes on a
 * background thread, control returns to the main loop thread via
 * worker_after_cb (which cleans up). After offloading, csilk_mq_next() is
 * called to continue the handler chain.
 *
 * @param ctx    Message queue context.
 * @param worker Worker function that will receive topic, payload, and length
 *               on a background thread.
 * @note The payload is deep-copied so the background thread can safely
 *       process it without worrying about mutex locking. */
void csilk_mq_offload(csilk_mq_ctx_t* ctx, csilk_mq_worker_t worker) {
  if (!ctx || !ctx->msg || !worker) return;
  csilk_mq_work_ctx_t* wctx = calloc(1, sizeof(csilk_mq_work_ctx_t));
  if (!wctx) return;
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
  uv_queue_work(ctx->mq->async_handle.loop, &wctx->req, worker_cb,
                worker_after_cb);
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
static int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload,
                       size_t len);

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
 * Allocates and initializes an MQ structure with a queue mutex, a libuv
 * async handle (for cross-thread signaling), and initializes WAL fields.
 * The async handle's callback (on_mq_async) processes queued messages.
 *
 * @param loop libuv event loop to associate the async handle with.
 * @return A new csilk_mq_t instance, or NULL on allocation failure.
 * @note The MQ must be freed via _csilk_mq_free(). The MQ is initially
 *       non-persistent — call csilk_mq_set_persistence() to enable WAL. */
csilk_mq_t* _csilk_mq_new(uv_loop_t* loop) {
  csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
  if (!mq) return NULL;
  uv_mutex_init(&mq->queue_mutex);
  uv_async_init(loop, &mq->async_handle, on_mq_async);
  mq->async_handle.data = mq;

  mq->wal_fd = -1;
  mq->wal_path = NULL;
  uv_mutex_init(&mq->wal_mutex);

  return mq;
}

/** @brief Enable persistent message delivery using a Write-Ahead Log (WAL).
 *
 * Opens (or creates) the given WAL file and registers it with the MQ.
 * When persistence is active, every published message is first appended to
 * the WAL before being enqueued in memory. On startup or after a crash,
 * existing messages in the WAL are recovered and re-enqueued automatically.
 * If a WAL was previously set, it is closed first.
 *
 * @param mq       The MQ instance.
 * @param wal_path File path for the WAL. The file is created if it does not
 *                 exist.
 * @return 0 on success, -1 if parameters are NULL or the file cannot be opened.
 * @note The WAL uses a simple binary format: [topic_len][topic][payload_len]
 *       [payload][checksum] entries. Checksum is a simple XOR for integrity. */
int csilk_mq_set_persistence(csilk_mq_t* mq, const char* wal_path) {
  if (!mq || !wal_path) return -1;

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
  int fd = uv_fs_open(mq->async_handle.loop, &open_req, wal_path,
                      O_CREAT | O_RDWR | O_APPEND, 0644, NULL);
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
static csilk_mq_topic_t* get_or_create_topic(csilk_mq_t* mq, const char* name) {
  for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
    if (strcmp(t->name, name) == 0) return t;
  }
  csilk_mq_topic_t* t = calloc(1, sizeof(csilk_mq_topic_t));
  if (!t) return NULL;
  t->name = strdup(name);
  t->next = mq->topics;
  mq->topics = t;
  return t;
}

/** @brief Register a middleware handler for a specific topic (or globally).
 *
 * If @p topic is NULL, the middleware is registered globally and applies to
 * ALL topics. Otherwise, it is registered for the specific topic. Middleware
 * handlers run before topic subscribers in the order they were registered.
 * Global middlewares run before topic-specific ones.
 *
 * @param mq        The MQ instance.
 * @param topic     Topic name (e.g., "user.created"), or NULL for global.
 * @param middleware Handler function to invoke during message processing.
 * @note The handler arrays grow dynamically (doubling capacity) as needed.
 *       Topic matching supports glob patterns via fnmatch(). */
void csilk_mq_use(csilk_mq_t* mq, const char* topic,
                  csilk_mq_handler_t middleware) {
  if (!mq || !middleware) return;
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
void csilk_mq_subscribe(csilk_mq_t* mq, const char* topic,
                        csilk_mq_handler_t subscriber) {
  /* Subscribers are essentially handlers appended to the chain */
  csilk_mq_use(mq, topic, subscriber);
}

/* --- Publishing and Async Dispatch --- */

/** @brief Internal: append a message frame to the Write-Ahead Log file.
 *
 * Writes a 5-part frame to the WAL: topic length (4 bytes), topic data
 * (N bytes), payload length (4 bytes), payload data (M bytes), and a
 * simple XOR checksum (4 bytes). After writing, the file is fsynced to
 * ensure durability.
 *
 * @param mq      The MQ instance (must have wal_fd >= 0).
 * @param topic   Message topic string.
 * @param payload Message payload data (may be NULL if len == 0).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 on write failure.
 * @note This is a no-op if the MQ has no WAL file (wal_fd < 0).
 * @note The caller should hold wal_mutex, though this function acquires it
 *       internally as well for safety. */
static int _mq_append_wal(csilk_mq_t* mq, const char* topic,
                          const void* payload, size_t len) {
  if (!mq || mq->wal_fd < 0 || !topic) return 0;

  uv_mutex_lock(&mq->wal_mutex);

  uint32_t topic_len = (uint32_t)strlen(topic);
  uint32_t payload_len = (uint32_t)len;
  uint32_t checksum = 0;

  /* Simple XOR checksum of topic and payload */
  for (uint32_t i = 0; i < topic_len; i++) checksum ^= (uint8_t)topic[i];
  const uint8_t* p = (const uint8_t*)payload;
  if (p) {
    for (uint32_t i = 0; i < payload_len; i++) checksum ^= p[i];
  }

  uv_buf_t bufs[5];
  bufs[0] = uv_buf_init((char*)&topic_len, 4);
  bufs[1] = uv_buf_init((char*)topic, topic_len);
  bufs[2] = uv_buf_init((char*)&payload_len, 4);
  bufs[3] = uv_buf_init((char*)payload, payload_len);
  bufs[4] = uv_buf_init((char*)&checksum, 4);

  uv_fs_t write_req;
  /* Write to the end of file (synchronous) */
  int result = uv_fs_write(mq->async_handle.loop, &write_req, mq->wal_fd, bufs,
                           5, -1, NULL);
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
 * Allocates a new csilk_mq_msg_t, copies the topic (via strdup) and payload
 * (via malloc+memcpy), appends it to the tail of the queue under the queue
 * mutex, and sends an async signal to wake up the event loop.
 *
 * @param mq      The MQ instance.
 * @param topic   Message topic.
 * @param payload Message payload (may be NULL).
 * @param len     Payload length.
 * @return 0 on success, -1 on allocation failure.
 * @note Thread-safe. The async signal ensures on_mq_async() processes the
 *       message on the main loop thread. */
static int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload,
                       size_t len) {
  csilk_mq_msg_t* msg = calloc(1, sizeof(csilk_mq_msg_t));
  if (!msg) return -1;
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
  uv_mutex_unlock(&mq->queue_mutex);

  uv_async_send(&mq->async_handle);
  return 0;
}

/** @brief Internal: recover messages from the Write-Ahead Log on startup.
 *
 * Reads the WAL file sequentially, parsing each frame (topic_len, topic,
 * payload_len, payload, checksum), validating the XOR checksum, and
 * re-enqueueing valid messages into the in-memory queue without re-appending
 * to the WAL. Stops at the first invalid frame (corruption or EOF).
 *
 * @param mq The MQ instance (must have wal_fd >= 0).
 * @return 0 on success (or if no WAL), -1 on allocation failure.
 * @note The WAL is NOT truncated after recovery — new messages append after
 *       existing ones. A future compaction step could truncate processed
 *       entries. */
static int _mq_recovery(csilk_mq_t* mq) {
  if (!mq || mq->wal_fd < 0) return 0;

  uint64_t offset = 0;
  while (1) {
    uint32_t topic_len = 0;
    uint32_t payload_len = 0;
    uint32_t checksum = 0;
    uv_fs_t read_req;
    int nread;

    /* 1. Read Topic Length */
    uv_buf_t buf = uv_buf_init((char*)&topic_len, 4);
    nread = uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1,
                       offset, NULL);
    uv_fs_req_cleanup(&read_req);
    if (nread < 4) break; /* EOF or error */
    offset += 4;

    /* 2. Read Topic Name */
    char* topic = malloc(topic_len + 1);
    if (!topic) break;
    buf = uv_buf_init(topic, topic_len);
    nread = uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1,
                       offset, NULL);
    uv_fs_req_cleanup(&read_req);
    if (nread < (int)topic_len) {
      free(topic);
      break;
    }
    topic[topic_len] = '\0';
    offset += topic_len;

    /* 3. Read Payload Length */
    buf = uv_buf_init((char*)&payload_len, 4);
    nread = uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1,
                       offset, NULL);
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
      nread = uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1,
                         offset, NULL);
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
    nread = uv_fs_read(mq->async_handle.loop, &read_req, mq->wal_fd, &buf, 1,
                       offset, NULL);
    uv_fs_req_cleanup(&read_req);
    if (nread < 4) {
      free(topic);
      free(payload);
      break;
    }
    offset += 4;

    /* 6. Validate Checksum */
    uint32_t calc_checksum = 0;
    for (uint32_t i = 0; i < topic_len; i++) calc_checksum ^= (uint8_t)topic[i];
    const uint8_t* p = (const uint8_t*)payload;
    if (p) {
      for (uint32_t i = 0; i < payload_len; i++) calc_checksum ^= p[i];
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
 * If WAL persistence is enabled, appends the message to the WAL first.
 * Then enqueues the message in the in-memory queue. Processing happens
 * asynchronously on the main event loop thread via on_mq_async().
 *
 * @param mq      The MQ instance.
 * @param topic   Target topic name (cannot be NULL).
 * @param payload Message payload data (may be NULL).
 * @param len     Payload length in bytes.
 * @return 0 on success, -1 if the topic is NULL or WAL append fails.
 * @note Thread-safe. The caller may free or reuse @p payload immediately
 *       after this call returns — the data is copied internally. */
int csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload,
                     size_t len) {
  if (!mq || !topic) return -1;

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
 * Drains the entire message queue under the queue mutex, then dispatches
 * each message through its matching handler chain. For each message, global
 * middlewares run first, followed by topic-specific handlers matched by
 * fnmatch(). The handler chain is allocated on the heap and freed after
 * dispatch.
 *
 * @param handle libuv async handle (data points to csilk_mq_t). */
static void on_mq_async(uv_async_t* handle) {
  csilk_mq_t* mq = (csilk_mq_t*)handle->data;

  uv_mutex_lock(&mq->queue_mutex);
  csilk_mq_msg_t* head = mq->queue_head;
  mq->queue_head = NULL;
  mq->queue_tail = NULL;
  uv_mutex_unlock(&mq->queue_mutex);

  while (head) {
    csilk_mq_msg_t* msg = head;
    head = head->next;

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
          memcpy(chain, mq->global_middlewares,
                 mq->global_mw_count * sizeof(csilk_mq_handler_t));
          idx += mq->global_mw_count;
        }

        for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
          if (fnmatch(t->name, msg->topic, 0) == 0 && t->handler_count > 0) {
            memcpy(chain + idx, t->handlers,
                   t->handler_count * sizeof(csilk_mq_handler_t));
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
 * Destroys mutexes, closes the WAL file (if open), frees the WAL path,
 * drains and frees the in-memory message queue, frees all topic structures
 * and their handlers, frees global middlewares, and finally frees the MQ
 * struct itself.
 *
 * @param handle libuv handle being closed (data points to csilk_mq_t). */
static void on_mq_close(uv_handle_t* handle) {
  csilk_mq_t* mq = (csilk_mq_t*)handle->data;
  if (!mq) return;

  uv_mutex_destroy(&mq->queue_mutex);
  uv_mutex_destroy(&mq->wal_mutex);

  if (mq->wal_fd >= 0) {
    uv_fs_t close_req;
    uv_fs_close(handle->loop, &close_req, mq->wal_fd, NULL);
    uv_fs_req_cleanup(&close_req);
  }
  if (mq->wal_path) free(mq->wal_path);

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
void _csilk_mq_free(csilk_mq_t* mq) {
  if (!mq) return;
  if (!uv_is_closing((uv_handle_t*)&mq->async_handle)) {
    mq->async_handle.data = mq;
    uv_close((uv_handle_t*)&mq->async_handle, on_mq_close);
  }
}
