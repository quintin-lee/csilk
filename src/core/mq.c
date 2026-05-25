/** @file mq.c
 * @brief Internal Event Bus (Message Queue) implementation.
 * @copyright MIT License
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fnmatch.h>
#include <fcntl.h>
#include "csilk.h"
#include "csilk_internal.h"

/* --- Context API --- */

/** @brief Proceed to the next middleware or subscriber in the MQ chain. */
void csilk_mq_next(csilk_mq_ctx_t* ctx) {
  if (!ctx || ctx->aborted) return;
  ctx->handler_index++;
  if (ctx->handler_index < (int)ctx->handler_count) {
    ctx->handlers[ctx->handler_index](ctx);
  }
}

/** @brief Abort the current MQ middleware chain. */
void csilk_mq_abort(csilk_mq_ctx_t* ctx) {
  if (ctx) ctx->aborted = 1;
}

/** @brief Get the topic of the current message in the MQ context. */
const char* csilk_mq_get_topic(csilk_mq_ctx_t* ctx) {
  return (ctx && ctx->msg) ? ctx->msg->topic : NULL;
}

/** @brief Get the payload of the current message in the MQ context. */
const void* csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len) {
  if (!ctx || !ctx->msg) return NULL;
  if (len) *len = ctx->msg->len;
  return ctx->msg->payload;
}

/* --- Offload API --- */

/** @brief Worker thread callback.
 * @param req libuv work request. */
static void worker_cb(uv_work_t* req) {
  csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
  wctx->handler(wctx->topic, wctx->payload, wctx->len);
}

/** @brief After work callback, executed on main loop thread.
 * @param req libuv work request.
 * @param status libuv status. */
static void worker_after_cb(uv_work_t* req, int status) {
  (void)status;
  csilk_mq_work_ctx_t* wctx = (csilk_mq_work_ctx_t*)req->data;
  free(wctx->topic);
  free(wctx->payload);
  free(wctx);
}

/** @brief Offload message processing to a background thread. */
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

/** @brief Internal: Enqueue a message in memory. */
static int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload,
                       size_t len);

/** @brief Internal: Recover messages from Write-Ahead Log. */
static int _mq_recovery(csilk_mq_t* mq);

/** @brief Internal: Create a new Message Queue instance. */
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

/** @brief Enable persistence for the Message Queue. */
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

/** @brief Register middleware for a topic. */
void csilk_mq_use(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t middleware) {
  if (!mq || !middleware) return;
  if (!topic) {
    if (mq->global_mw_count >= mq->global_mw_capacity) {
      size_t new_cap = mq->global_mw_capacity ? mq->global_mw_capacity * 2 : 4;
      mq->global_middlewares = realloc(mq->global_middlewares, new_cap * sizeof(csilk_mq_handler_t));
      mq->global_mw_capacity = new_cap;
    }
    mq->global_middlewares[mq->global_mw_count++] = middleware;
  } else {
    csilk_mq_topic_t* t = get_or_create_topic(mq, topic);
    if (t) {
      if (t->handler_count >= t->handler_capacity) {
        size_t new_cap = t->handler_capacity ? t->handler_capacity * 2 : 4;
        t->handlers = realloc(t->handlers, new_cap * sizeof(csilk_mq_handler_t));
        t->handler_capacity = new_cap;
      }
      t->handlers[t->handler_count++] = middleware;
    }
  }
}

/** @brief Register a subscriber for a topic. */
void csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber) {
  /* Subscribers are essentially handlers appended to the chain */
  csilk_mq_use(mq, topic, subscriber);
}

/* --- Publishing and Async Dispatch --- */

/** @brief Append a message to the Write-Ahead Log.
 * @param mq The MQ instance.
 * @param topic Message topic.
 * @param payload Message payload.
 * @param len Payload length.
 * @return 0 on success, -1 on failure. */
static int _mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload,
                          size_t len) {
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

/** @brief Internal: Enqueue a message in memory. */
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

/** @brief Internal: Recover messages from Write-Ahead Log.
 * @param mq The MQ instance.
 * @return 0 on success, -1 on failure. */
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

/** @brief Publish a message to a topic (Thread-safe). */
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

/** @brief Async callback to process all queued MQ messages.
 * @param handle libuv async handle. */
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
      csilk_mq_handler_t* chain = malloc(total_handlers * sizeof(csilk_mq_handler_t));
      if (chain) {
        size_t idx = 0;
        if (mq->global_mw_count > 0) {
          memcpy(chain, mq->global_middlewares, mq->global_mw_count * sizeof(csilk_mq_handler_t));
          idx += mq->global_mw_count;
        }
        
        for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
          if (fnmatch(t->name, msg->topic, 0) == 0 && t->handler_count > 0) {
            memcpy(chain + idx, t->handlers, t->handler_count * sizeof(csilk_mq_handler_t));
            idx += t->handler_count;
          }
        }
        
        csilk_mq_ctx_t ctx = { mq, msg, chain, total_handlers, -1, 0 };
        csilk_mq_next(&ctx);
        free(chain);
      }
    }
    
    free(msg->topic);
    free(msg->payload);
    free(msg);
  }
}

/** @brief libuv close callback for the MQ async handle.
 * @param handle libuv handle. */
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

/** @brief Internal: Free Message Queue instance and its resources. */
void _csilk_mq_free(csilk_mq_t* mq) {
  if (!mq) return;
  if (!uv_is_closing((uv_handle_t*)&mq->async_handle)) {
    mq->async_handle.data = mq;
    uv_close((uv_handle_t*)&mq->async_handle, on_mq_close);
  }
}
