/**
 * @file ai.c
 * @brief AI unified interface engine implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/drivers/ai.h"

#define MAX_DRIVERS 8

static const csilk_ai_driver_t* g_drivers[MAX_DRIVERS];
static size_t g_driver_count = 0;

struct csilk_ai_s {
  const csilk_ai_driver_t* driver;
  void* driver_state;
};

void csilk_ai_register_driver(const csilk_ai_driver_t* driver) {
  if (g_driver_count < MAX_DRIVERS) {
    g_drivers[g_driver_count++] = driver;
  }
}

static const csilk_ai_driver_t* find_driver(const char* name) {
  for (size_t i = 0; i < g_driver_count; i++) {
    if (strcmp(g_drivers[i]->name, name) == 0) {
      return g_drivers[i];
    }
  }
  return NULL;
}

/* Forward declarations for default drivers */
extern void csilk_ai_openai_init_driver(void);
extern void csilk_ai_ollama_init_driver(void);

csilk_ai_t* csilk_ai_new(const char* driver_name, const char* api_key,
                         const char* base_url) {
  /* Auto-initialize default drivers on first use */
  static int initialized = 0;
  if (!initialized) {
    csilk_ai_openai_init_driver();
    csilk_ai_ollama_init_driver();
    initialized = 1;
  }

  const csilk_ai_driver_t* driver = find_driver(driver_name);
  if (!driver) return NULL;

  void* state = driver->init(api_key, base_url);
  if (!state) return NULL;

  csilk_ai_t* ai = malloc(sizeof(csilk_ai_t));
  if (!ai) {
    driver->free(state);
    return NULL;
  }

  ai->driver = driver;
  ai->driver_state = state;
  return ai;
}

int csilk_ai_chat(csilk_ai_t* ai, const csilk_ai_chat_request_t* req,
                  csilk_ai_chat_response_t* res) {
  if (!ai || !req || !res) return -1;

  int retries = 0;
  int max_retries = 2; /* Default max retries */
  int status = -1;

  while (retries <= max_retries) {
    memset(res, 0, sizeof(*res));
    status = ai->driver->chat(ai->driver_state, req, res);

    if (status == 0) break;

    /* Only retry on specific errors (e.g., transient network or 429) 
       For now, simple retry on any error if content is NULL */
    if (res->error_message && 
        (strstr(res->error_message, "CURL error") || 
         strstr(res->error_message, "429") ||
         strstr(res->error_message, "502") ||
         strstr(res->error_message, "503"))) {
      retries++;
      if (retries <= max_retries) {
        /* Exponential backoff */
        unsigned int wait_ms = (unsigned int)(1000 * (1 << (retries - 1)));
        uv_sleep(wait_ms);
        csilk_ai_chat_response_free(res);
        continue;
      }
    }
    break;
  }

  return status;
}

typedef struct {
  csilk_ai_t* ai;
  const csilk_ai_chat_request_t* req;
  csilk_ai_chat_response_t res;
  csilk_ai_chat_async_cb cb;
  void* user_data;
  int status;
} async_chat_req_t;

static void chat_work_cb(uv_work_t* req) {
  async_chat_req_t* ar = (async_chat_req_t*)req->data;
  ar->status = csilk_ai_chat(ar->ai, ar->req, &ar->res);
}

static void chat_after_work_cb(uv_work_t* req, int status) {
  (void)status;
  async_chat_req_t* ar = (async_chat_req_t*)req->data;
  ar->cb(ar->status, &ar->res, ar->user_data);
  free(ar);
  free(req);
}

void csilk_ai_chat_async(csilk_ai_t* ai, const csilk_ai_chat_request_t* req,
                         csilk_ai_chat_async_cb cb, void* user_data) {
  if (!ai || !req || !cb) return;

  uv_work_t* work = malloc(sizeof(uv_work_t));
  async_chat_req_t* ar = malloc(sizeof(async_chat_req_t));
  if (!work || !ar) {
    free(work);
    free(ar);
    return;
  }

  ar->ai = ai;
  ar->req = req;
  ar->cb = cb;
  ar->user_data = user_data;
  memset(&ar->res, 0, sizeof(ar->res));

  work->data = ar;
  uv_queue_work(uv_default_loop(), work, chat_work_cb, chat_after_work_cb);
}

int csilk_ai_embeddings(csilk_ai_t* ai, const char* model, const char** input,
                        size_t count, csilk_ai_embeddings_response_t* res) {
  if (!ai || !model || !input || !res) return -1;
  memset(res, 0, sizeof(*res));
  if (!ai->driver->embeddings) {
    res->error_message = strdup("Driver does not support embeddings");
    return -1;
  }
  return ai->driver->embeddings(ai->driver_state, model, input, count, res);
}

typedef struct {
  csilk_ai_t* ai;
  const char* model;
  const char** input;
  size_t count;
  csilk_ai_embeddings_response_t res;
  csilk_ai_embeddings_async_cb cb;
  void* user_data;
  int status;
} async_emb_req_t;

static void emb_work_cb(uv_work_t* req) {
  async_emb_req_t* ar = (async_emb_req_t*)req->data;
  ar->status =
      csilk_ai_embeddings(ar->ai, ar->model, ar->input, ar->count, &ar->res);
}

static void emb_after_work_cb(uv_work_t* req, int status) {
  (void)status;
  async_emb_req_t* ar = (async_emb_req_t*)req->data;
  ar->cb(ar->status, &ar->res, ar->user_data);
  free(ar);
  free(req);
}

void csilk_ai_embeddings_async(csilk_ai_t* ai, const char* model,
                               const char** input, size_t count,
                               csilk_ai_embeddings_async_cb cb,
                               void* user_data) {
  if (!ai || !model || !input || !cb) return;

  uv_work_t* work = malloc(sizeof(uv_work_t));
  async_emb_req_t* ar = malloc(sizeof(async_emb_req_t));
  if (!work || !ar) {
    free(work);
    free(ar);
    return;
  }

  ar->ai = ai;
  ar->model = model;
  ar->input = input;
  ar->count = count;
  ar->cb = cb;
  ar->user_data = user_data;
  memset(&ar->res, 0, sizeof(ar->res));

  work->data = ar;
  uv_queue_work(uv_default_loop(), work, emb_work_cb, emb_after_work_cb);
}

void csilk_ai_free(csilk_ai_t* ai) {
  if (!ai) return;
  ai->driver->free(ai->driver_state);
  free(ai);
}

void csilk_ai_chat_response_free(csilk_ai_chat_response_t* res) {
  if (!res) return;
  free(res->content);
  if (res->tool_calls) {
    for (size_t i = 0; i < res->tool_call_count; i++) {
      free(res->tool_calls[i].id);
      free(res->tool_calls[i].name);
      free(res->tool_calls[i].arguments);
    }
    free(res->tool_calls);
  }
  free(res->raw_response);
  free(res->error_message);
}

void csilk_ai_embeddings_response_free(csilk_ai_embeddings_response_t* res) {
  if (!res) return;
  free(res->values);
  free(res->error_message);
}

/* --- Context Helpers Implementation --- */

csilk_ai_context_t* csilk_ai_context_new(size_t max_history) {
  csilk_ai_context_t* ctx = calloc(1, sizeof(csilk_ai_context_t));
  if (ctx) {
    ctx->max_history = max_history;
  }
  return ctx;
}

void csilk_ai_context_add(csilk_ai_context_t* ctx, const char* role,
                         const char* content) {
  if (!ctx || !role || !content) return;

  if (ctx->count >= ctx->capacity) {
    size_t new_cap = ctx->capacity == 0 ? 8 : ctx->capacity * 2;
    csilk_ai_message_t* new_msgs =
        realloc(ctx->messages, sizeof(csilk_ai_message_t) * new_cap);
    if (!new_msgs) return;
    ctx->messages = new_msgs;
    ctx->capacity = new_cap;
  }

  /* Sliding window check */
  if (ctx->max_history > 0 && ctx->count >= ctx->max_history) {
    /* Remove oldest message */
    free((char*)ctx->messages[0].role);
    free((char*)ctx->messages[0].content);
    memmove(&ctx->messages[0], &ctx->messages[1],
            sizeof(csilk_ai_message_t) * (ctx->count - 1));
    ctx->count--;
  }

  ctx->messages[ctx->count].role = strdup(role);
  ctx->messages[ctx->count].content = strdup(content);
  ctx->count++;
}

void csilk_ai_context_clear(csilk_ai_context_t* ctx) {
  if (!ctx) return;
  for (size_t i = 0; i < ctx->count; i++) {
    free((char*)ctx->messages[i].role);
    free((char*)ctx->messages[i].content);
  }
  ctx->count = 0;
}

void csilk_ai_context_free(csilk_ai_context_t* ctx) {
  if (!ctx) return;
  csilk_ai_context_clear(ctx);
  free(ctx->messages);
  free(ctx);
}
