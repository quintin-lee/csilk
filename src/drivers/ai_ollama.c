/**
 * @file ai_ollama.c
 * @brief Ollama driver for the AI unified interface.
 * @copyright MIT License
 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/drivers/ai.h"

typedef struct {
  char* base_url;
} ollama_state_t;

static void* ollama_init(const char* api_key, const char* base_url) {
  (void)api_key; /* Ollama usually doesn't need a key locally */
  ollama_state_t* state = malloc(sizeof(ollama_state_t));
  if (!state) return NULL;

  state->base_url = strdup(base_url ? base_url : "http://localhost:11434");

  static int curl_init = 0;
  if (!curl_init) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_init = 1;
  }

  return state;
}

static void ollama_free(void* state_ptr) {
  ollama_state_t* state = (ollama_state_t*)state_ptr;
  if (!state) return;
  free(state->base_url);
  free(state);
}

struct curl_response {
  char* body;
  size_t size;
};

static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
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

static int ollama_chat(void* state_ptr, const csilk_ai_chat_request_t* req,
                      csilk_ai_chat_response_t* res) {
  ollama_state_t* state = (ollama_state_t*)state_ptr;
  CURL* curl = curl_easy_init();
  if (!curl) return -1;

  /* Build JSON request body (Ollama /api/chat format) */
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", req->model ? req->model : "llama3");
  
  cJSON* msgs = cJSON_CreateArray();
  for (size_t i = 0; i < req->message_count; i++) {
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", req->messages[i].role);
    cJSON_AddStringToObject(m, "content", req->messages[i].content);
    cJSON_AddItemToArray(msgs, m);
  }
  cJSON_AddItemToObject(root, "messages", msgs);

  cJSON_AddBoolToObject(root, "stream", false); /* Simplified for now */

  cJSON* opts = cJSON_CreateObject();
  if (req->temperature > 0) cJSON_AddNumberToObject(opts, "temperature", req->temperature);
  if (req->top_p > 0) cJSON_AddNumberToObject(opts, "top_p", req->top_p);
  cJSON_AddItemToObject(root, "options", opts);

  char* json_body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "%s/api/chat", state->base_url);

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  struct curl_response cr = {0};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&cr);

  CURLcode rc = curl_easy_perform(curl);
  free(json_body);
  curl_slist_free_all(headers);

  if (rc != CURLE_OK) {
    res->error_message = strdup(curl_easy_strerror(rc));
    free(cr.body);
    curl_easy_cleanup(curl);
    return -1;
  }

  /* Parse Ollama response */
  cJSON* resp_root = cJSON_Parse(cr.body);
  if (resp_root) {
    cJSON* msg = cJSON_GetObjectItem(resp_root, "message");
    cJSON* content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content)) {
      res->content = strdup(content->valuestring);
    }
    res->prompt_tokens = cJSON_GetObjectItem(resp_root, "prompt_eval_count")->valueint;
    res->completion_tokens = cJSON_GetObjectItem(resp_root, "eval_count")->valueint;
    res->total_tokens = res->prompt_tokens + res->completion_tokens;
    cJSON_Delete(resp_root);
  }
  
  free(cr.body);
  curl_easy_cleanup(curl);
  return res->content ? 0 : -1;
}

static int ollama_embeddings(void* state_ptr, const char* model,
                            const char** input, size_t count,
                            csilk_ai_embeddings_response_t* res) {
  (void)state_ptr; (void)model; (void)input; (void)count; (void)res;
  /* TODO: Implement Ollama embeddings */
  return -1;
}

static const csilk_ai_driver_t ollama_driver = {
    .name = "ollama",
    .init = ollama_init,
    .chat = ollama_chat,
    .embeddings = ollama_embeddings,
    .free = ollama_free,
};

void csilk_ai_ollama_init_driver(void) {
  csilk_ai_register_driver(&ollama_driver);
}
