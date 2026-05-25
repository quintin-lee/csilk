#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "csilk.h"
#include "csilk_internal.h"

/* --- Context API --- */

void csilk_mq_next(csilk_mq_ctx_t* ctx) {
  if (!ctx || ctx->aborted) return;
  ctx->handler_index++;
  if (ctx->handler_index < (int)ctx->handler_count) {
    ctx->handlers[ctx->handler_index](ctx);
  }
}

void csilk_mq_abort(csilk_mq_ctx_t* ctx) {
  if (ctx) ctx->aborted = 1;
}

const char* csilk_mq_get_topic(csilk_mq_ctx_t* ctx) {
  return (ctx && ctx->msg) ? ctx->msg->topic : NULL;
}

const void* csilk_mq_get_payload(csilk_mq_ctx_t* ctx, size_t* len) {
  if (!ctx || !ctx->msg) return NULL;
  if (len) *len = ctx->msg->len;
  return ctx->msg->payload;
}

/* --- Setup API --- */

static void on_mq_async(uv_async_t* handle);

csilk_mq_t* _csilk_mq_new(uv_loop_t* loop) {
  csilk_mq_t* mq = calloc(1, sizeof(csilk_mq_t));
  if (!mq) return NULL;
  uv_mutex_init(&mq->queue_mutex);
  uv_async_init(loop, &mq->async_handle, on_mq_async);
  mq->async_handle.data = mq;
  return mq;
}

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

void csilk_mq_subscribe(csilk_mq_t* mq, const char* topic, csilk_mq_handler_t subscriber) {
  /* Subscribers are essentially handlers appended to the chain */
  csilk_mq_use(mq, topic, subscriber);
}

/* --- Publishing and Async Dispatch --- */

int csilk_mq_publish(csilk_mq_t* mq, const char* topic, const void* payload, size_t len) {
  if (!mq || !topic) return -1;
  csilk_mq_msg_t* msg = calloc(1, sizeof(csilk_mq_msg_t));
  if (!msg) return -1;
  msg->topic = strdup(topic);
  if (len > 0 && payload) {
    msg->payload = malloc(len);
    if (!msg->payload) { free(msg->topic); free(msg); return -1; }
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
    
    csilk_mq_topic_t* target = NULL;
    for (csilk_mq_topic_t* t = mq->topics; t; t = t->next) {
      if (strcmp(t->name, msg->topic) == 0) { target = t; break; }
    }
    
    if (target || mq->global_mw_count > 0) {
      size_t total_handlers = mq->global_mw_count + (target ? target->handler_count : 0);
      csilk_mq_handler_t* chain = malloc(total_handlers * sizeof(csilk_mq_handler_t));
      if (chain) {
        if (mq->global_mw_count > 0) memcpy(chain, mq->global_middlewares, mq->global_mw_count * sizeof(csilk_mq_handler_t));
        if (target && target->handler_count > 0) memcpy(chain + mq->global_mw_count, target->handlers, target->handler_count * sizeof(csilk_mq_handler_t));
        
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

static void on_mq_close(uv_handle_t* handle) {
  csilk_mq_t* mq = (csilk_mq_t*)handle->data;
  if (!mq) return;

  uv_mutex_destroy(&mq->queue_mutex);

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

void _csilk_mq_free(csilk_mq_t* mq) {
  if (!mq) return;
  if (!uv_is_closing((uv_handle_t*)&mq->async_handle)) {
    mq->async_handle.data = mq;
    uv_close((uv_handle_t*)&mq->async_handle, on_mq_close);
  }
}
