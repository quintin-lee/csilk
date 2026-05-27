/**
 * @file ai_openai.c
 * @brief OpenAI driver for the AI unified interface.
 * @copyright MIT License
 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/drivers/ai.h"

typedef struct {
  char* api_key;
  char* base_url;
} openai_state_t;

static void* openai_init(const char* api_key, const char* base_url) {
  if (!api_key) return NULL;
  openai_state_t* state = malloc(sizeof(openai_state_t));
  if (!state) return NULL;

  state->api_key = strdup(api_key);
  state->base_url = strdup(base_url ? base_url : "https://api.openai.com/v1");

  /* Initialize CURL global state if needed (libcurl docs recommend doing this
   * once) */
  static int curl_init = 0;
  if (!curl_init) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_init = 1;
  }

  return state;
}

static void openai_free(void* state_ptr) {
  openai_state_t* state = (openai_state_t*)state_ptr;
  if (!state) return;
  free(state->api_key);
  free(state->base_url);
  free(state);
}

struct curl_response {
  char* body;
  size_t size;
};

struct curl_context {
  const csilk_ai_chat_request_t* req;
  csilk_ai_chat_response_t* res;
  char* body;
  size_t size;
  char* line_buf;
  size_t line_size;
};

static size_t write_cb_simple(void* contents, size_t size, size_t nmemb,
                              void* userp) {
  size_t realsize = size * nmemb;
  struct curl_response* res = (struct curl_response*)userp;

  char* ptr = realloc(res->body, res->size + realsize + 1);
  if (!ptr) return 0;

  res->body = ptr;
  memcpy(&(res->body[res->size]), contents, realsize);
  res->size += realsize;
  res->body[res->size] = 0;

  return realsize;
}

static void process_stream_line(struct curl_context* ctx, const char* line) {
  if (strncmp(line, "data: ", 5) != 0) return;
  const char* data = line + 6;
  if (strcmp(data, "[DONE]") == 0) return;

  cJSON* root = cJSON_Parse(data);
  if (!root) return;

  cJSON* choices = cJSON_GetObjectItem(root, "choices");
  if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
    cJSON* first = cJSON_GetArrayItem(choices, 0);
    cJSON* delta = cJSON_GetObjectItem(first, "delta");
    cJSON* content = cJSON_GetObjectItem(delta, "content");
    if (cJSON_IsString(content)) {
      /* Append to full content */
      size_t clen = strlen(content->valuestring);
      size_t current_len = ctx->res->content ? strlen(ctx->res->content) : 0;
      char* new_content = realloc(ctx->res->content, current_len + clen + 1);
      if (new_content) {
        ctx->res->content = new_content;
        memcpy(ctx->res->content + current_len, content->valuestring, clen);
        ctx->res->content[current_len + clen] = '\0';
      }

      /* Call user callback */
      if (ctx->req->on_chunk) {
        ctx->req->on_chunk(content->valuestring, ctx->req->user_data);
      }
    }
  }
  cJSON_Delete(root);
}

static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t realsize = size * nmemb;
  struct curl_context* ctx = (struct curl_context*)userp;

  if (ctx->req->stream) {
    /* Process SSE stream */
    char* ptr = realloc(ctx->line_buf, ctx->line_size + realsize + 1);
    if (!ptr) return 0;
    ctx->line_buf = ptr;
    memcpy(ctx->line_buf + ctx->line_size, contents, realsize);
    ctx->line_size += realsize;
    ctx->line_buf[ctx->line_size] = '\0';

    char* line_start = ctx->line_buf;
    char* newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
      *newline = '\0';
      process_stream_line(ctx, line_start);
      line_start = newline + 1;
    }

    /* Keep remaining partial line */
    size_t processed = (size_t)(line_start - ctx->line_buf);
    size_t remaining = ctx->line_size - processed;
    if (remaining > 0) {
      memmove(ctx->line_buf, line_start, remaining);
    }
    ctx->line_size = remaining;
    ctx->line_buf[ctx->line_size] = '\0';
  } else {
    /* Regular non-stream accumulation */
    char* ptr = realloc(ctx->body, ctx->size + realsize + 1);
    if (!ptr) return 0;
    ctx->body = ptr;
    memcpy(&(ctx->body[ctx->size]), contents, realsize);
    ctx->size += realsize;
    ctx->body[ctx->size] = 0;
  }

  return realsize;
}

static int openai_chat(void* state_ptr, const csilk_ai_chat_request_t* req,
                       csilk_ai_chat_response_t* res) {
  openai_state_t* state = (openai_state_t*)state_ptr;
  CURL* curl = curl_easy_init();
  if (!curl) return -1;

  /* Build JSON request body */
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model",
                          req->model ? req->model : "gpt-3.5-turbo");

  cJSON* msgs = cJSON_CreateArray();
  for (size_t i = 0; i < req->message_count; i++) {
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", req->messages[i].role);
    cJSON_AddStringToObject(m, "content", req->messages[i].content);
    cJSON_AddItemToArray(msgs, m);
  }
  cJSON_AddItemToObject(root, "messages", msgs);

  if (req->temperature > 0)
    cJSON_AddNumberToObject(root, "temperature", req->temperature);
  if (req->top_p > 0) cJSON_AddNumberToObject(root, "top_p", req->top_p);
  if (req->presence_penalty != 0)
    cJSON_AddNumberToObject(root, "presence_penalty", req->presence_penalty);
  if (req->frequency_penalty != 0)
    cJSON_AddNumberToObject(root, "frequency_penalty", req->frequency_penalty);
  if (req->max_tokens > 0)
    cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens);
  if (req->user) cJSON_AddStringToObject(root, "user", req->user);
  if (req->stream) cJSON_AddBoolToObject(root, "stream", true);

  if (req->stop_count > 0) {
    cJSON* stop = cJSON_CreateArray();
    for (size_t i = 0; i < req->stop_count; i++) {
      cJSON_AddItemToArray(stop, cJSON_CreateString(req->stop[i]));
    }
    cJSON_AddItemToObject(root, "stop", stop);
  }

  if (req->tool_count > 0) {
    cJSON* tools = cJSON_CreateArray();
    for (size_t i = 0; i < req->tool_count; i++) {
      cJSON* t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "type", req->tools[i].type);
      cJSON* f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "name", req->tools[i].function.name);
      if (req->tools[i].function.description) {
        cJSON_AddStringToObject(f, "description", req->tools[i].function.description);
      }
      if (req->tools[i].function.parameters_json) {
        cJSON_AddItemToObject(f, "parameters", cJSON_Duplicate((cJSON*)req->tools[i].function.parameters_json, 1));
      }
      cJSON_AddItemToObject(t, "function", f);
      cJSON_AddItemToArray(tools, t);
    }
    cJSON_AddItemToObject(root, "tools", tools);
    if (req->tool_choice) {
      cJSON_AddStringToObject(root, "tool_choice", req->tool_choice);
    }
  }

  char* json_body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "%s/chat/completions", state->base_url);

  struct curl_slist* headers = NULL;
  char auth_hdr[256];
  snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s",
           state->api_key);
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_hdr);

  struct curl_context ctx = {0};
  ctx.req = req;
  ctx.res = res;

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&ctx);

  if (req->timeout_ms > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
  }

  CURLcode rc = curl_easy_perform(curl);
  free(json_body);
  curl_slist_free_all(headers);
  free(ctx.line_buf);

  if (rc != CURLE_OK) {
    char err_buf[256];
    snprintf(err_buf, sizeof(err_buf), "CURL error (%d): %s", rc,
             curl_easy_strerror(rc));
    res->error_message = strdup(err_buf);
    free(ctx.body);
    curl_easy_cleanup(curl);
    return -1;
  }

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code >= 400) {
    char err_buf[512];
    snprintf(err_buf, sizeof(err_buf), "HTTP error %ld: %s", http_code,
             ctx.body ? ctx.body : "(no response body)");
    res->error_message = strdup(err_buf);
    free(ctx.body);
    curl_easy_cleanup(curl);
    return -1;
  }

  if (req->stream) {
    /* Final chunk build complete in write_cb */
    curl_easy_cleanup(curl);
    return res->content ? 0 : -1;
  }

  /* Parse full response for non-stream */
  cJSON* resp_root = cJSON_Parse(ctx.body);
  if (resp_root) {
    res->raw_response = ctx.body;
    cJSON* choices = cJSON_GetObjectItem(resp_root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
      cJSON* first = cJSON_GetArrayItem(choices, 0);
      cJSON* msg = cJSON_GetObjectItem(first, "message");
      cJSON* content = cJSON_GetObjectItem(msg, "content");
      if (cJSON_IsString(content)) {
        res->content = strdup(content->valuestring);
      }
      cJSON* tcalls = cJSON_GetObjectItem(msg, "tool_calls");
      if (cJSON_IsArray(tcalls)) {
        res->tool_call_count = cJSON_GetArraySize(tcalls);
        res->tool_calls =
            calloc(res->tool_call_count, sizeof(csilk_ai_tool_call_t));
        for (size_t i = 0; i < res->tool_call_count; i++) {
          cJSON* tc = cJSON_GetArrayItem(tcalls, i);
          cJSON* fid = cJSON_GetObjectItem(tc, "id");
          cJSON* func = cJSON_GetObjectItem(tc, "function");
          cJSON* fname = cJSON_GetObjectItem(func, "name");
          cJSON* fargs = cJSON_GetObjectItem(func, "arguments");

          if (fid) res->tool_calls[i].id = strdup(fid->valuestring);
          if (fname) res->tool_calls[i].name = strdup(fname->valuestring);
          if (fargs) res->tool_calls[i].arguments = strdup(fargs->valuestring);
        }
      }
    }
    cJSON* usage = cJSON_GetObjectItem(resp_root, "usage");
    if (usage) {
      res->prompt_tokens =
          cJSON_GetObjectItem(usage, "prompt_tokens")->valueint;
      res->completion_tokens =
          cJSON_GetObjectItem(usage, "completion_tokens")->valueint;
      res->total_tokens = cJSON_GetObjectItem(usage, "total_tokens")->valueint;
    }
    cJSON_Delete(resp_root);
  } else {
    free(ctx.body);
  }

  curl_easy_cleanup(curl);
  return res->content ? 0 : -1;
}

static int openai_embeddings(void* state_ptr, const char* model,
                             const char** input, size_t count,
                             csilk_ai_embeddings_response_t* res) {
  openai_state_t* state = (openai_state_t*)state_ptr;
  CURL* curl = curl_easy_init();
  if (!curl) return -1;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", model);
  cJSON* in_arr = cJSON_CreateArray();
  for (size_t i = 0; i < count; i++) {
    cJSON_AddItemToArray(in_arr, cJSON_CreateString(input[i]));
  }
  cJSON_AddItemToObject(root, "input", in_arr);

  char* json_body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "%s/embeddings", state->base_url);

  struct curl_slist* headers = NULL;
  char auth_hdr[256];
  snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s",
           state->api_key);
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_hdr);

  struct curl_response cr = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb_simple);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);

  CURLcode rc = curl_easy_perform(curl);
  free(json_body);
  curl_slist_free_all(headers);

  if (rc != CURLE_OK) {
    char err[256];
    snprintf(err, sizeof(err), "CURL error: %s", curl_easy_strerror(rc));
    res->error_message = strdup(err);
    free(cr.body);
    curl_easy_cleanup(curl);
    return -1;
  }

  cJSON* resp = cJSON_Parse(cr.body);
  free(cr.body);
  if (!resp) {
    res->error_message = strdup("JSON parse error");
    curl_easy_cleanup(curl);
    return -1;
  }

  cJSON* data = cJSON_GetObjectItem(resp, "data");
  if (cJSON_IsArray(data)) {
    res->count = cJSON_GetArraySize(data);
    if (res->count > 0) {
      cJSON* first = cJSON_GetArrayItem(data, 0);
      cJSON* vec = cJSON_GetObjectItem(first, "embedding");
      if (cJSON_IsArray(vec)) {
        res->dimension = cJSON_GetArraySize(vec);
        res->values = malloc(sizeof(float) * res->count * res->dimension);
        for (size_t i = 0; i < res->count; i++) {
          cJSON* item = cJSON_GetArrayItem(data, i);
          cJSON* v = cJSON_GetObjectItem(item, "embedding");
          for (size_t j = 0; j < res->dimension; j++) {
            res->values[i * res->dimension + j] =
                (float)cJSON_GetArrayItem(v, j)->valuedouble;
          }
        }
      }
    }
  }

  cJSON* usage = cJSON_GetObjectItem(resp, "usage");
  if (usage) {
    res->prompt_tokens = cJSON_GetObjectItem(usage, "prompt_tokens")->valueint;
    res->total_tokens = cJSON_GetObjectItem(usage, "total_tokens")->valueint;
  }

  cJSON_Delete(resp);
  curl_easy_cleanup(curl);
  return 0;
}

static const csilk_ai_driver_t openai_driver = {
    .name = "openai",
    .init = openai_init,
    .chat = openai_chat,
    .embeddings = openai_embeddings,
    .free = openai_free,
};

void csilk_ai_openai_init_driver(void) {
  csilk_ai_register_driver(&openai_driver);
}
