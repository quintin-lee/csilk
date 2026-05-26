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
#include "csilk_ai.h"

typedef struct {
  char* api_key;
  char* base_url;
} openai_state_t;

static void* openai_init(const char* api_key, const char* base_url) {
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

static int openai_chat(void* state_ptr, const csilk_ai_chat_request_t* req,
                       csilk_ai_chat_response_t* res) {
  openai_state_t* state = (openai_state_t*)state_ptr;
  CURL* curl = curl_easy_init();
  if (!curl) return -1;

  /* Build JSON request body */
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", req->model ? req->model : "gpt-3.5-turbo");
  
  cJSON* msgs = cJSON_CreateArray();
  for (size_t i = 0; i < req->message_count; i++) {
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", req->messages[i].role);
    cJSON_AddStringToObject(m, "content", req->messages[i].content);
    cJSON_AddItemToArray(msgs, m);
  }
  cJSON_AddItemToObject(root, "messages", msgs);

  if (req->temperature > 0) cJSON_AddNumberToObject(root, "temperature", req->temperature);
  if (req->max_tokens > 0) cJSON_AddNumberToObject(root, "max_tokens", req->max_tokens);
  if (req->stream) cJSON_AddBoolToObject(root, "stream", true);

  char* json_body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char url[512];
  snprintf(url, sizeof(url), "%s/chat/completions", state->base_url);

  struct curl_slist* headers = NULL;
  char auth_hdr[256];
  snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", state->api_key);
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth_hdr);

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
    free(cr.body);
    curl_easy_cleanup(curl);
    return -1;
  }

  /* Parse response */
  cJSON* resp_root = cJSON_Parse(cr.body);
  if (resp_root) {
    res->raw_response = cr.body;
    cJSON* choices = cJSON_GetObjectItem(resp_root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
      cJSON* first = cJSON_GetArrayItem(choices, 0);
      cJSON* msg = cJSON_GetObjectItem(first, "message");
      cJSON* content = cJSON_GetObjectItem(msg, "content");
      if (cJSON_IsString(content)) {
        res->content = strdup(content->valuestring);
      }
    }
    cJSON* usage = cJSON_GetObjectItem(resp_root, "usage");
    if (usage) {
      res->prompt_tokens = cJSON_GetObjectItem(usage, "prompt_tokens")->valueint;
      res->completion_tokens = cJSON_GetObjectItem(usage, "completion_tokens")->valueint;
      res->total_tokens = cJSON_GetObjectItem(usage, "total_tokens")->valueint;
    }
    cJSON_Delete(resp_root);
  } else {
    free(cr.body);
  }

  curl_easy_cleanup(curl);
  return res->content ? 0 : -1;
}

static const csilk_ai_driver_t openai_driver = {
    .name = "openai",
    .init = openai_init,
    .chat = openai_chat,
    .free = openai_free,
};

void csilk_ai_openai_init_driver(void) {
  csilk_ai_register_driver(&openai_driver);
}
