/**
 * @file workflow.c
 * @brief AI Workflow engine implementation with WAL persistence, Tracing, Tools, Monitoring, and Budgeting.
 */

#include "csilk/app/workflow.h"
#include "csilk/core/internal.h"
#include "workflow_wal.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <stdio.h>
#include <time.h>

#define MAX_WORKFLOW_STEPS 1000

typedef struct {
  char* model;
  int prompt_tokens;
  int completion_tokens;
} csilk_ai_meta_t;

typedef struct {
  char* name;
  char* description;
  char* parameters_json;
  csilk_wf_tool_fn fn;
  void* user_data;
} csilk_wf_tool_entry_t;

typedef struct csilk_wf_edge_s {
  char* condition;           /**< NULL for default/bind. */
  csilk_wf_node_t* target;   /**< Destination node. */
} csilk_wf_edge_t;

struct csilk_wf_node_s {
  char* id;
  int index;                 /**< Internal index for tracking in context. */
  csilk_wf_handler_t handler;
  void* user_data;
  
  csilk_wf_edge_t* edges;
  size_t edge_count;
  size_t edge_capacity;
  
  int incoming_count;        /**< Number of incoming edges. */
  int is_entry;              /**< Explicitly marked as entry node. */
  
  csilk_wf_node_t* error_target;  /**< Fallback node on failure. */
  csilk_wf_router_t router_fn;    /**< Dynamic router callback. */
  csilk_wf_join_policy_t join_policy; /**< AND or OR. */
  int timeout_ms;            /**< Node execution timeout. */
};

struct csilk_wf_s {
  char* name;
  csilk_wf_node_t** nodes;
  size_t node_count;
  size_t node_capacity;
  uv_loop_t* loop;
  char* wal_dir;             /**< Persistence directory. */
  
  csilk_wf_tool_entry_t* tools; /**< Registered tools. */
  size_t tool_count;
  size_t tool_capacity;
  
  csilk_ctx_t** monitors;    /**< WebSocket connections for monitoring. */
  size_t monitor_count;
  size_t monitor_capacity;
  uv_mutex_t monitor_mutex;
  
  int max_tokens;            /**< Maximum total tokens allowed. */
  int ttl_sec;               /**< Workflow Time-To-Live. */
};

struct csilk_wf_ctx_s {
  csilk_wf_t* wf;
  csilk_data_t* initial_input;
  void (*callback)(csilk_data_t*);
  void (*trace_callback)(csilk_data_t*, csilk_wf_trace_t*);
  
  int* node_input_counts;    /**< Tracking received inputs per node index. */
  int total_executions;      /**< Safety counter to prevent infinite loops. */
  int nodes_active;          /**< Number of nodes currently running or queued. */
  uv_mutex_t mutex;          /**< Protects scheduler state. */
  
  csilk_arena_t* arena;      /**< Memory arena for this execution. */
  uv_mutex_t arena_mutex;    /**< Protects arena from parallel allocations. */
  
  csilk_data_t** node_outputs; /**< History of outputs per node index. */
  
  char exec_id[37];          /**< Unique execution identifier. */
  char* wal_path;            /**< Full path to the WAL file. */
  
  csilk_wf_trace_t* trace;   /**< Execution history. */
  uv_mutex_t trace_mutex;    /**< Protects trace appends. */
  
  int total_tokens;          /**< Cumulative tokens used. */
  int is_terminated;         /**< Hard stop flag (e.g., budget exceeded). */

  uv_timer_t ttl_timer;      /**< Global TTL timer. */
  int is_ttl_expired;        /**< TTL expiration flag. */
};

typedef struct node_work_s {
  uv_work_t req;
  csilk_wf_ctx_t* ctx;
  csilk_wf_node_t* node;
  csilk_data_t* input;
  csilk_data_t* output;
  csilk_wf_trace_node_t* trace_node;
  uv_timer_t node_timer;     /**< Per-node timeout timer. */
  int is_timed_out;          /**< Timeout flag. */
} node_work_t;

/* --- Internal Helpers --- */
static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);
static void cleanup_ctx(csilk_wf_ctx_t* ctx);
static const char* csilk_wf_run_ext_internal(csilk_wf_t* wf, csilk_data_t* input,
                                            void (*callback)(csilk_data_t*),
                                            void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*));

/* --- Lifecycle --- */

csilk_wf_t* csilk_wf_new(const char* name) {
  csilk_wf_t* wf = calloc(1, sizeof(csilk_wf_t));
  if (wf) {
    wf->name = strdup(name);
    wf->loop = uv_default_loop();
    uv_mutex_init(&wf->monitor_mutex);
  }
  return wf;
}

static void node_free(csilk_wf_node_t* node) {
  if (!node) return;
  free(node->id);
  for (size_t i = 0; i < node->edge_count; i++) {
    free(node->edges[i].condition);
  }
  free(node->edges);
  free(node);
}

void csilk_wf_free(csilk_wf_t* wf) {
  if (!wf) return;
  free(wf->name);
  free(wf->wal_dir);
  for (size_t i = 0; i < wf->node_count; i++) {
    node_free(wf->nodes[i]);
  }
  free(wf->nodes);
  for (size_t i = 0; i < wf->tool_count; i++) {
      free(wf->tools[i].name);
      free(wf->tools[i].description);
      free(wf->tools[i].parameters_json);
  }
  free(wf->tools);
  uv_mutex_destroy(&wf->monitor_mutex);
  free(wf->monitors);
  free(wf);
}

csilk_wf_node_t* csilk_wf_add(csilk_wf_t* wf, const char* id,
                              csilk_wf_handler_t handler, void* user_data) {
  if (!wf || !id || !handler) return NULL;
  if (wf->node_count >= wf->node_capacity) {
    size_t new_cap = wf->node_capacity == 0 ? 8 : wf->node_capacity * 2;
    csilk_wf_node_t** new_nodes = realloc(wf->nodes, sizeof(csilk_wf_node_t*) * new_cap);
    if (!new_nodes) return NULL;
    wf->nodes = new_nodes;
    wf->node_capacity = new_cap;
  }
  csilk_wf_node_t* node = calloc(1, sizeof(csilk_wf_node_t));
  if (!node) return NULL;
  node->id = strdup(id);
  node->index = (int)wf->node_count;
  node->handler = handler;
  node->user_data = user_data;
  node->join_policy = CSILK_WF_JOIN_AND;
  wf->nodes[wf->node_count++] = node;
  return node;
}

void csilk_wf_node_set_entry(csilk_wf_node_t* node, int is_entry) {
  if (node) node->is_entry = is_entry;
}

/* --- Connections --- */

static void node_add_edge(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to, int is_loop) {
  if (!from || !to) return;
  if (from->edge_count >= from->edge_capacity) {
    size_t new_cap = from->edge_capacity == 0 ? 4 : from->edge_capacity * 2;
    csilk_wf_edge_t* new_edges = realloc(from->edges, sizeof(csilk_wf_edge_t) * new_cap);
    if (!new_edges) return;
    from->edges = new_edges;
    from->edge_capacity = new_cap;
  }
  from->edges[from->edge_count].condition = condition ? strdup(condition) : NULL;
  from->edges[from->edge_count].target = to;
  from->edge_count++;
  if (!is_loop) { to->incoming_count++; }
}

void csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to) { node_add_edge(from, NULL, to, 0); }
void csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) { node_add_edge(from, condition, to, 0); }
void csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) { node_add_edge(from, condition, to, 1); }
void csilk_wf_on_error(csilk_wf_node_t* from, csilk_wf_node_t* target) { if (from) from->error_target = target; }
void csilk_wf_route(csilk_wf_node_t* node, csilk_wf_router_t router) { if (node) node->router_fn = router; }
void csilk_wf_node_set_join(csilk_wf_node_t* node, csilk_wf_join_policy_t policy) { if (node) node->join_policy = policy; }

/* --- Persistence --- */

void csilk_wf_set_persistence(csilk_wf_t* wf, const char* wal_dir) {
  if (!wf) return;
  free(wf->wal_dir);
  wf->wal_dir = wal_dir ? strdup(wal_dir) : NULL;
}

/* --- Monitoring --- */

void csilk_wf_register_monitor(csilk_wf_t* wf, csilk_ctx_t* c) {
    if (!wf || !c) return;
    uv_mutex_lock(&wf->monitor_mutex);
    if (wf->monitor_count >= wf->monitor_capacity) {
        size_t new_cap = wf->monitor_capacity == 0 ? 4 : wf->monitor_capacity * 2;
        csilk_ctx_t** new_monitors = realloc(wf->monitors, sizeof(csilk_ctx_t*) * new_cap);
        if (new_monitors) {
            wf->monitors = new_monitors;
            wf->monitor_capacity = new_cap;
        }
    }
    if (wf->monitor_count < wf->monitor_capacity) {
        wf->monitors[wf->monitor_count++] = c;
    }
    uv_mutex_unlock(&wf->monitor_mutex);
}

static void _wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload) {
    if (!wf || wf->monitor_count == 0) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event);
    if (node_id) cJSON_AddStringToObject(root, "node_id", node_id);
    if (payload) cJSON_AddStringToObject(root, "payload", payload);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    char* json = cJSON_PrintUnformatted(root);
    uv_mutex_lock(&wf->monitor_mutex);
    for (size_t i = 0; i < wf->monitor_count; i++) csilk_ws_send(wf->monitors[i], (uint8_t*)json, strlen(json), 0x1);
    uv_mutex_unlock(&wf->monitor_mutex);
    free(json); cJSON_Delete(root);
}

/* --- Budget --- */

void csilk_wf_set_budget(csilk_wf_t* wf, int max_tokens) {
    if (wf) wf->max_tokens = max_tokens;
}

void csilk_wf_node_set_timeout(csilk_wf_node_t* node, int timeout_ms) {
    if (node) node->timeout_ms = timeout_ms;
}

void csilk_wf_set_ttl(csilk_wf_t* wf, int ttl_sec) {
    if (wf) wf->ttl_sec = ttl_sec;
}

/* --- Memory Helpers --- */

void* csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size) {
  if (!ctx) return NULL;
  uv_mutex_lock(&ctx->arena_mutex);
  void* ptr = csilk_arena_alloc(ctx->arena, size);
  uv_mutex_unlock(&ctx->arena_mutex);
  return ptr;
}

char* csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char* news = csilk_wf_alloc(ctx, len + 1);
  if (news) { memcpy(news, s, len + 1); }
  return news;
}

csilk_data_t* csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value) {
  csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
  if (data) {
    data->type = csilk_wf_strdup(ctx, type);
    data->value = value;
    data->free_fn = NULL;
    data->meta = NULL;
  }
  return data;
}

static char* _csilk_json_get_path(csilk_wf_ctx_t* ctx, cJSON* root, const char* path) {
    if (!root || !path) return NULL;
    
    cJSON* curr = root;
    char* path_copy = strdup(path);
    char* saveptr;
    char* token = strtok_r(path_copy, ".", &saveptr);
    
    while (token && curr) {
        if (cJSON_IsArray(curr)) {
            curr = cJSON_GetArrayItem(curr, atoi(token));
        } else {
            curr = cJSON_GetObjectItemCaseSensitive(curr, token);
        }
        token = strtok_r(NULL, ".", &saveptr);
    }
    
    char* result = NULL;
    if (curr) {
        if (cJSON_IsString(curr)) {
            result = csilk_wf_strdup(ctx, curr->valuestring);
        } else if (cJSON_IsNumber(curr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", curr->valuedouble);
            result = csilk_wf_strdup(ctx, buf);
        } else if (cJSON_IsBool(curr)) {
            result = csilk_wf_strdup(ctx, curr->valueint ? "true" : "false");
        } else if (cJSON_IsNull(curr)) {
            result = csilk_wf_strdup(ctx, "null");
        } else {
            // For objects/arrays, return as stringified JSON
            char* tmp = cJSON_PrintUnformatted(curr);
            result = csilk_wf_strdup(ctx, tmp);
            free(tmp);
        }
    }
    
    free(path_copy);
    return result;
}

/* --- Template Engine & AI Node --- */

static char* resolve_templates(csilk_wf_ctx_t* ctx, const char* template) {
  if (!template) return NULL;
  char* res = csilk_wf_strdup(ctx, template);
  
  // 1. Handle node references: {{node_id.value}} or {{node_id.value.path.to.field}}
  for (size_t i = 0; i < ctx->wf->node_count; i++) {
    csilk_wf_node_t* n = ctx->wf->nodes[i];
    char base_pattern[256];
    snprintf(base_pattern, sizeof(base_pattern), "{{%s.value", n->id);
    
    char* pos;
    while ((pos = strstr(res, base_pattern)) != NULL) {
      char* end = strstr(pos, "}}");
      if (!end) break;
      
      size_t pat_full_len = end - pos + 2;
      char* replacement = "(null)";
      csilk_data_t* out = ctx->node_outputs[n->index];
      
      if (out && out->value) {
        // Check if there's a path after .value
        // pos points to "{{node_id.value"
        // we need to see if it's followed by "." or "}}"
        char* path_start = pos + strlen(base_pattern);
        if (*path_start == '.') {
          // JSONPath!
          path_start++; // Skip the dot
          size_t path_len = end - path_start;
          char* path = malloc(path_len + 1);
          memcpy(path, path_start, path_len);
          path[path_len] = '\0';
          
          cJSON* json = cJSON_Parse((char*)out->value);
          if (json) {
            char* val = _csilk_json_get_path(ctx, json, path);
            if (val) replacement = val;
            cJSON_Delete(json);
          }
          free(path);
        } else if (*path_start == '}') {
          // Pure value
          replacement = (char*)out->value;
        }
      }
      
      size_t rep_len = strlen(replacement);
      size_t res_len = strlen(res);
      char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
      size_t prefix_len = pos - res;
      memcpy(new_res, res, prefix_len);
      memcpy(new_res + prefix_len, replacement, rep_len);
      strcpy(new_res + prefix_len + rep_len, end + 2);
      res = new_res;
    }
  }
  
  // 2. Handle initial input: {{input.value}} or {{input.value.path}}
  const char* in_pattern = "{{input.value";
  char* pos;
  while ((pos = strstr(res, in_pattern)) != NULL) {
      char* end = strstr(pos, "}}");
      if (!end) break;
      
      size_t pat_full_len = end - pos + 2;
      char* replacement = "(null)";
      
      if (ctx->initial_input && ctx->initial_input->value) {
          char* path_start = pos + strlen(in_pattern);
          if (*path_start == '.') {
              path_start++;
              size_t path_len = end - path_start;
              char* path = malloc(path_len + 1);
              memcpy(path, path_start, path_len);
              path[path_len] = '\0';
              
              cJSON* json = cJSON_Parse((char*)ctx->initial_input->value);
              if (json) {
                  char* val = _csilk_json_get_path(ctx, json, path);
                  if (val) replacement = val;
                  cJSON_Delete(json);
              }
              free(path);
          } else if (*path_start == '}') {
              replacement = (char*)ctx->initial_input->value;
          }
      }
      
      size_t rep_len = strlen(replacement);
      size_t res_len = strlen(res);
      char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
      size_t prefix_len = pos - res;
      memcpy(new_res, res, prefix_len);
      memcpy(new_res + prefix_len, replacement, rep_len);
      strcpy(new_res + prefix_len + rep_len, end + 2);
      res = new_res;
  }
  
  return res;
}

static csilk_data_t* ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
  (void)input; csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
  char* prompt = resolve_templates(ctx, config->prompt);
  const char* api_key = getenv("AGENT_API_KEY");
  if (!api_key) return NULL;
  csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
  if (!ai) return NULL;
  csilk_ai_tool_t* tools = NULL;
  if (ctx->wf->tool_count > 0) {
      tools = calloc(ctx->wf->tool_count, sizeof(csilk_ai_tool_t));
      for (size_t i = 0; i < ctx->wf->tool_count; i++) {
          tools[i].type = "function"; tools[i].function.name = ctx->wf->tools[i].name;
          tools[i].function.description = ctx->wf->tools[i].description;
          if (ctx->wf->tools[i].parameters_json) tools[i].function.parameters_json = cJSON_Parse(ctx->wf->tools[i].parameters_json);
      }
  }
  size_t msg_capacity = 16; csilk_ai_message_t* msgs = calloc(msg_capacity, sizeof(csilk_ai_message_t)); size_t msg_count = 0;
  if (config->system_msg) { msgs[msg_count].role = "system"; msgs[msg_count].content = strdup(config->system_msg); msg_count++; }
  msgs[msg_count].role = "user"; msgs[msg_count].content = strdup(prompt); msg_count++;
  csilk_data_t* out = NULL; int iterations = 0;
  while (iterations < 10) {
      iterations++;
      csilk_ai_chat_request_t req = { .model = config->model ? config->model : "gpt-3.5-turbo", .messages = msgs, .message_count = msg_count, .temperature = config->temperature > 0 ? config->temperature : 0.7, .max_tokens = config->max_tokens > 0 ? config->max_tokens : 1024, .tools = tools, .tool_count = ctx->wf->tool_count };
      csilk_ai_chat_response_t res;
      if (csilk_ai_chat(ai, &req, &res) != 0) break;
      if (res.tool_call_count > 0) {
          if (msg_count + res.tool_call_count + 1 >= msg_capacity) { msg_capacity *= 2; msgs = realloc(msgs, sizeof(csilk_ai_message_t) * msg_capacity); }
          msgs[msg_count].role = "assistant"; msgs[msg_count].content = res.content ? strdup(res.content) : strdup(""); msg_count++;
          for (size_t i = 0; i < res.tool_call_count; i++) {
              csilk_ai_tool_call_t* tc = &res.tool_calls[i]; char* result_json = NULL;
              for (size_t j = 0; j < ctx->wf->tool_count; j++) if (strcmp(ctx->wf->tools[j].name, tc->name) == 0) { result_json = ctx->wf->tools[j].fn(tc->arguments, ctx->wf->tools[j].user_data); break; }
              msgs[msg_count].role = "tool"; msgs[msg_count].content = result_json ? result_json : strdup("{}"); msg_count++;
          }
          csilk_ai_chat_response_free(&res); continue;
      }
      out = csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, res.content));
      csilk_ai_meta_t* meta = csilk_wf_alloc(ctx, sizeof(csilk_ai_meta_t));
      meta->model = csilk_wf_strdup(ctx, req.model); meta->prompt_tokens = res.prompt_tokens; meta->completion_tokens = res.completion_tokens;
      out->meta = meta;
      csilk_ai_chat_response_free(&res); break;
  }
  for (size_t i = 0; i < msg_count; i++) free((void*)msgs[i].content);
  free(msgs);
  if (tools) { for (size_t i = 0; i < ctx->wf->tool_count; i++) cJSON_Delete(tools[i].function.parameters_json); free(tools); }
  csilk_ai_free(ai); return out;
}

csilk_wf_node_t* csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config) {
  csilk_ai_config_t* copy = malloc(sizeof(csilk_ai_config_t));
  memcpy(copy, config, sizeof(csilk_ai_config_t));
  copy->model = config->model ? strdup(config->model) : NULL;
  copy->system_msg = config->system_msg ? strdup(config->system_msg) : NULL;
  copy->prompt = config->prompt ? strdup(config->prompt) : NULL;
  return csilk_wf_add(wf, id, ai_node_handler, copy);
}

void csilk_wf_register_tool(csilk_wf_t* wf, const char* name, const char* description,
                            const char* parameters_json, csilk_wf_tool_fn fn, void* user_data) {
    if (!wf || !name || !fn) return;
    uv_mutex_lock(&wf->monitor_mutex);
    if (wf->tool_count >= wf->tool_capacity) {
        size_t new_cap = wf->tool_capacity == 0 ? 4 : wf->tool_capacity * 2;
        csilk_wf_tool_entry_t* new_tools = realloc(wf->tools, sizeof(csilk_wf_tool_entry_t) * new_cap);
        if (new_tools) { wf->tools = new_tools; wf->tool_capacity = new_cap; }
    }
    if (wf->tool_count < wf->tool_capacity) {
        csilk_wf_tool_entry_t* entry = &wf->tools[wf->tool_count++];
        entry->name = strdup(name); entry->description = description ? strdup(description) : NULL;
        entry->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        entry->fn = fn; entry->user_data = user_data;
    }
    uv_mutex_unlock(&wf->monitor_mutex);
}

csilk_wf_node_t* csilk_wf_get_node(csilk_wf_t* wf, const char* id) {
  if (!wf || !id) return NULL;
  for (size_t i = 0; i < wf->node_count; i++) if (strcmp(wf->nodes[i]->id, id) == 0) return wf->nodes[i];
  return NULL;
}

/* --- Visualization --- */

char* csilk_wf_to_mermaid(csilk_wf_t* wf) {
  if (!wf) return NULL;
  size_t buf_size = 8192; char* buf = malloc(buf_size); strcpy(buf, "graph TD\n");
  for (size_t i = 0; i < wf->node_count; i++) {
    csilk_wf_node_t* n = wf->nodes[i]; char line[512]; snprintf(line, sizeof(line), "  %s[%s]\n", n->id, n->id); strcat(buf, line);
    for (size_t j = 0; j < n->edge_count; j++) {
      csilk_wf_edge_t* e = &n->edges[j];
      if (e->condition) snprintf(line, sizeof(line), "  %s -- %s --> %s\n", n->id, e->condition, e->target->id);
      else snprintf(line, sizeof(line), "  %s --> %s\n", n->id, e->target->id);
      strcat(buf, line);
    }
    if (n->error_target) { snprintf(line, sizeof(line), "  %s -. error .-> %s\n", n->id, n->error_target->id); strcat(buf, line); }
  }
  return buf;
}

/* --- Tracing Implementation --- */

char* csilk_wf_trace_to_json(const csilk_wf_trace_t* trace) {
    if (!trace) return NULL;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "exec_id", trace->exec_id);
    cJSON_AddNumberToObject(root, "start_time", (double)trace->start_time);
    cJSON_AddNumberToObject(root, "end_time", (double)trace->end_time);
    cJSON_AddNumberToObject(root, "duration_us", (double)(trace->end_time - trace->start_time));
    cJSON* nodes = cJSON_CreateArray();
    for (size_t i = 0; i < trace->node_count; i++) {
        csilk_wf_trace_node_t* n = trace->nodes[i];
        cJSON* nj = cJSON_CreateObject();
        cJSON_AddStringToObject(nj, "node_id", n->node_id);
        cJSON_AddNumberToObject(nj, "start_time", (double)n->start_time);
        cJSON_AddNumberToObject(nj, "end_time", (double)n->end_time);
        cJSON_AddNumberToObject(nj, "duration_us", (double)(n->end_time - n->start_time));
        if (n->input_dump) cJSON_AddStringToObject(nj, "input", n->input_dump);
        if (n->output_dump) cJSON_AddStringToObject(nj, "output", n->output_dump);
        if (n->model) cJSON_AddStringToObject(nj, "model", n->model);
        if (n->prompt_tokens > 0) cJSON_AddNumberToObject(nj, "prompt_tokens", n->prompt_tokens);
        if (n->completion_tokens > 0) cJSON_AddNumberToObject(nj, "completion_tokens", n->completion_tokens);
        if (n->error) cJSON_AddStringToObject(nj, "error", n->error);
        cJSON_AddItemToArray(nodes, nj);
    }
    cJSON_AddItemToObject(root, "nodes", nodes);
    char* out = cJSON_Print(root); cJSON_Delete(root); return out;
}

void csilk_wf_trace_free(csilk_wf_trace_t* trace) {
    if (!trace) return; free(trace->exec_id);
    for (size_t i = 0; i < trace->node_count; i++) {
        csilk_wf_trace_node_t* n = trace->nodes[i];
        free(n->node_id); free(n->input_dump); free(n->output_dump); free(n->model); free(n->error); free(n);
    }
    free(trace->nodes); free(trace);
}

/* --- Scheduler Implementation --- */

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);

static void cleanup_ctx(csilk_wf_ctx_t* ctx) {
  if (!ctx) return;
  if (ctx->wf->ttl_sec > 0) {
      uv_timer_stop(&ctx->ttl_timer);
      if (!uv_is_closing((uv_handle_t*)&ctx->ttl_timer)) {
          uv_close((uv_handle_t*)&ctx->ttl_timer, NULL);
      }
  }
  uv_mutex_destroy(&ctx->mutex); uv_mutex_destroy(&ctx->arena_mutex); uv_mutex_destroy(&ctx->trace_mutex);
  csilk_arena_free(ctx->arena); free(ctx->node_input_counts); free(ctx->node_outputs); free(ctx->wal_path);
  free(ctx);
}

static void wal_log_event(csilk_wf_ctx_t* ctx, csilk_wf_event_type_t type, const char* node_id, csilk_data_t* data) {
    if (!ctx->wal_path) return;
    size_t node_id_len = node_id ? strlen(node_id) + 1 : 0;
    size_t type_len = (data && data->type) ? strlen(data->type) + 1 : 0;
    size_t val_len = (data && data->value) ? strlen((char*)data->value) + 1 : 0;
    size_t total_len = node_id_len + type_len + val_len;
    char* payload = malloc(total_len); char* p = payload;
    if (node_id_len) { memcpy(p, node_id, node_id_len); p += node_id_len; }
    if (type_len) { memcpy(p, data->type, type_len); p += type_len; }
    if (val_len) { memcpy(p, data->value, val_len); }
    _wf_wal_append(ctx->wal_path, type, payload, total_len);
    free(payload);
}

static void worker_cb(uv_work_t* req) {
  node_work_t* work = (node_work_t*)req->data;
  _wf_broadcast(work->ctx->wf, "node_start", work->node->id, NULL);
  work->output = work->node->handler(work->ctx, work->input, work->node->user_data);
}

static void after_worker_cb(uv_work_t* req, int status) {
  (void)status;
  node_work_t* work = (node_work_t*)req->data;
  csilk_wf_ctx_t* ctx = work->ctx; csilk_wf_node_t* node = work->node; csilk_data_t* output = work->output;

  if (node->timeout_ms > 0) {
      uv_timer_stop(&work->node_timer);
  }

  if (work->is_timed_out) {
      output = NULL;
  }

  uv_mutex_lock(&ctx->mutex);
  ctx->node_outputs[node->index] = output;
  if (output && output->meta) {
      csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
      ctx->total_tokens += am->prompt_tokens + am->completion_tokens;
  }
  uv_mutex_unlock(&ctx->mutex);

  wal_log_event(ctx, WF_EV_NODE_FINISH, node->id, output);
  _wf_broadcast(ctx->wf, "node_finish", node->id, output ? (char*)output->value : NULL);

  if (work->trace_node) {
      work->trace_node->end_time = uv_hrtime() / 1000;
      if (output) {
          work->trace_node->output_dump = strdup(output->value ? (char*)output->value : "(null)");
          if (output->meta) {
              csilk_ai_meta_t* am = (csilk_ai_meta_t*)output->meta;
              work->trace_node->model = strdup(am->model);
              work->trace_node->prompt_tokens = am->prompt_tokens; work->trace_node->completion_tokens = am->completion_tokens;
          }
      }
      uv_mutex_lock(&ctx->trace_mutex);
      ctx->trace->nodes = realloc(ctx->trace->nodes, sizeof(csilk_wf_trace_node_t*) * (ctx->trace->node_count + 1));
      ctx->trace->nodes[ctx->trace->node_count++] = work->trace_node;
      uv_mutex_unlock(&ctx->trace_mutex);
  }

  uv_mutex_lock(&ctx->mutex);
  if (ctx->wf->max_tokens > 0 && ctx->total_tokens > ctx->wf->max_tokens) {
      ctx->is_terminated = 1;
      printf("[Workflow] Budget exceeded: %d > %d. Terminating.\n", ctx->total_tokens, ctx->wf->max_tokens);
  }
  int terminated = ctx->is_terminated;
  uv_mutex_unlock(&ctx->mutex);

  if (terminated) {
      uv_mutex_lock(&ctx->mutex);
      ctx->nodes_active--;
      int current_active = ctx->nodes_active;
      uv_mutex_unlock(&ctx->mutex);
      if (current_active == 0) {
          if (ctx->trace_callback) ctx->trace_callback(NULL, ctx->trace);
          else if (ctx->callback) ctx->callback(NULL);
          if (ctx->trace_callback) ctx->trace = NULL;
          cleanup_ctx(ctx);
      }
      free(work); return;
  }

  if (output == NULL && node->error_target) {
    execute_node(ctx, node->error_target, NULL);
    uv_mutex_lock(&ctx->mutex); ctx->nodes_active--; uv_mutex_unlock(&ctx->mutex);
    free(work); return;
  }

  int triggered_count = 0;
  if (node->router_fn) {
    const char* target_id = node->router_fn(output);
    if (target_id) {
      for (size_t i = 0; i < ctx->wf->node_count; i++) if (strcmp(ctx->wf->nodes[i]->id, target_id) == 0) { execute_node(ctx, ctx->wf->nodes[i], output); triggered_count++; break; }
    }
  } else {
    for (size_t i = 0; i < node->edge_count; i++) {
      csilk_wf_edge_t* edge = &node->edges[i]; int match = 0;
      if (edge->condition == NULL) match = 1;
      else if (output && output->type && strcmp(output->type, edge->condition) == 0) match = 1;
      if (match) {
        csilk_wf_node_t* target = edge->target; int ready = 0;
        uv_mutex_lock(&ctx->mutex);
        if (ctx->total_executions < MAX_WORKFLOW_STEPS) {
          ctx->node_input_counts[target->index]++;
          int threshold = target->incoming_count == 0 ? 1 : target->incoming_count;
          if (ctx->node_input_counts[target->index] >= threshold) { ready = 1; ctx->node_input_counts[target->index] = 0; }
        }
        uv_mutex_unlock(&ctx->mutex);
        if (ready) { execute_node(ctx, target, output); triggered_count++; }
      }
    }
  }
  
  uv_mutex_lock(&ctx->mutex); ctx->nodes_active--; int current_active = ctx->nodes_active; uv_mutex_unlock(&ctx->mutex);
  if (triggered_count == 0 && current_active == 0) {
    wal_log_event(ctx, WF_EV_END, NULL, NULL);
    _wf_broadcast(ctx->wf, "workflow_end", NULL, output ? (char*)output->value : NULL);
    if (ctx->trace) ctx->trace->end_time = uv_hrtime() / 1000;
    if (ctx->trace_callback) ctx->trace_callback(output, ctx->trace); else if (ctx->callback) ctx->callback(output);
    if (ctx->trace_callback) ctx->trace = NULL; else if (ctx->trace) { csilk_wf_trace_free(ctx->trace); ctx->trace = NULL; }
    cleanup_ctx(ctx);
  }
  free(work);
}

static void on_node_timeout(uv_timer_t* handle) {
  node_work_t* work = (node_work_t*)handle->data;
  work->is_timed_out = 1;
  printf("[Workflow] Node '%s' timed out after %dms\n", work->node->id, work->node->timeout_ms);
}

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input) {
  uv_mutex_lock(&ctx->mutex);
  if (ctx->is_terminated) { uv_mutex_unlock(&ctx->mutex); return; }
  uv_mutex_unlock(&ctx->mutex);

  wal_log_event(ctx, WF_EV_NODE_START, node->id, NULL);
  _wf_broadcast(ctx->wf, "node_queued", node->id, NULL);

  node_work_t* work = calloc(1, sizeof(node_work_t));
  if (ctx->trace) {
      csilk_wf_trace_node_t* tn = calloc(1, sizeof(csilk_wf_trace_node_t));
      tn->node_id = strdup(node->id); tn->start_time = uv_hrtime() / 1000;
      tn->input_dump = strdup(input && input->value ? (char*)input->value : "(null)");
      work->trace_node = tn;
  }

  uv_mutex_lock(&ctx->mutex);
  ctx->total_executions++;
  ctx->nodes_active++;
  uv_mutex_unlock(&ctx->mutex);

  work->req.data = work;
  work->ctx = ctx;
  work->node = node;
  work->input = input;

  if (node->timeout_ms > 0) {
      uv_timer_init(ctx->wf->loop, &work->node_timer);
      work->node_timer.data = work;
      uv_timer_start(&work->node_timer, on_node_timeout, node->timeout_ms, 0);
  }

  uv_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

const char* csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result)) {
    return csilk_wf_run_ext_internal(wf, input, callback, NULL);
}

void csilk_wf_run_traced(csilk_wf_t* wf, csilk_data_t* input, void (*callback)(csilk_data_t* result, csilk_wf_trace_t* trace)) {
    csilk_wf_run_ext_internal(wf, input, NULL, callback);
}

static void on_workflow_ttl(uv_timer_t* handle) {
    csilk_wf_ctx_t* ctx = (csilk_wf_ctx_t*)handle->data;
    uv_mutex_lock(&ctx->mutex);
    ctx->is_terminated = 1;
    ctx->is_ttl_expired = 1;
    uv_mutex_unlock(&ctx->mutex);
    printf("[Workflow] TTL Expired for execution %s\n", ctx->exec_id);
}

static const char* csilk_wf_run_ext_internal(csilk_wf_t* wf, csilk_data_t* input,
                                            void (*callback)(csilk_data_t*),
                                            void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*)) {
  if (!wf || wf->node_count == 0) { if (callback) callback(NULL); if (trace_cb) trace_cb(NULL, NULL); return NULL; }
  csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
  ctx->wf = wf; ctx->initial_input = input; ctx->callback = callback; ctx->trace_callback = trace_cb;
  ctx->node_input_counts = calloc(wf->node_count, sizeof(int)); ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
  ctx->arena = csilk_arena_new(0); uv_mutex_init(&ctx->mutex); uv_mutex_init(&ctx->arena_mutex); uv_mutex_init(&ctx->trace_mutex);
  csilk_generate_uuid(ctx->exec_id);
  
  if (wf->ttl_sec > 0) {
      uv_timer_init(wf->loop, &ctx->ttl_timer);
      ctx->ttl_timer.data = ctx;
      uv_timer_start(&ctx->ttl_timer, on_workflow_ttl, wf->ttl_sec * 1000, 0);
  }

  _wf_broadcast(wf, "workflow_start", ctx->exec_id, input ? (char*)input->value : NULL);
  if (wf->wal_dir) {
      char path[512]; snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, ctx->exec_id); ctx->wal_path = strdup(path);
      wal_log_event(ctx, WF_EV_START, NULL, input);
  }
  if (trace_cb) { ctx->trace = calloc(1, sizeof(csilk_wf_trace_t)); ctx->trace->exec_id = strdup(ctx->exec_id); ctx->trace->start_time = uv_hrtime() / 1000; }
  int started = 0;
  for (size_t i = 0; i < wf->node_count; i++) if (wf->nodes[i]->is_entry) { execute_node(ctx, wf->nodes[i], input); started = 1; }
  if (!started) for (size_t i = 0; i < wf->node_count; i++) if (wf->nodes[i]->incoming_count == 0) { execute_node(ctx, wf->nodes[i], input); started = 1; }
  if (!started) { if (callback) callback(NULL); if (trace_cb) trace_cb(NULL, NULL); cleanup_ctx(ctx); return NULL; }
  return ctx->exec_id;
}

void csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result)) {
    if (!wf || !exec_id || !wf->wal_dir) return;
    char wal_path[512]; snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
    FILE* f = fopen(wal_path, "rb"); if (!f) return;
    csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
    ctx->wf = wf; ctx->callback = callback; ctx->node_input_counts = calloc(wf->node_count, sizeof(int)); ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
    ctx->arena = csilk_arena_new(0); uv_mutex_init(&ctx->mutex); uv_mutex_init(&ctx->arena_mutex); uv_mutex_init(&ctx->trace_mutex);
    strcpy(ctx->exec_id, exec_id); ctx->wal_path = strdup(wal_path);
    int* node_started = calloc(wf->node_count, sizeof(int)), *node_finished = calloc(wf->node_count, sizeof(int)); int wf_ended = 0;
    csilk_wf_wal_header_t header;
    while (fread(&header, sizeof(header), 1, f) == 1) {
        if (header.magic != CSILK_WF_MAGIC) break;
        char* payload = header.payload_len > 0 ? malloc(header.payload_len) : NULL;
        if (payload && fread(payload, header.payload_len, 1, f) != 1) { free(payload); break; }
        switch (header.type) {
            case WF_EV_NODE_START: for (size_t i = 0; i < wf->node_count; i++) if (strcmp(wf->nodes[i]->id, payload) == 0) { node_started[i] = 1; break; } break;
            case WF_EV_NODE_FINISH: {
                char* node_id = payload; char* data_type = node_id + strlen(node_id) + 1; char* data_val = data_type + strlen(data_type) + 1;
                for (size_t i = 0; i < wf->node_count; i++) if (strcmp(wf->nodes[i]->id, node_id) == 0) {
                    node_finished[i] = 1; ctx->node_outputs[i] = csilk_wf_data_new(ctx, data_type, csilk_wf_strdup(ctx, data_val));
                    csilk_wf_node_t* n = wf->nodes[i]; for (size_t j = 0; j < n->edge_count; j++) ctx->node_input_counts[n->edges[j].target->index]++; break;
                } break;
            }
            case WF_EV_END: wf_ended = 1; break;
        }
        free(payload);
    }
    fclose(f);
    if (wf_ended) { if (callback) callback(NULL); cleanup_ctx(ctx); }
    else {
        for (size_t i = 0; i < wf->node_count; i++) {
            csilk_wf_node_t* n = wf->nodes[i]; int trigger = 0; csilk_data_t* trigger_input = ctx->initial_input;
            if (node_started[i] && !node_finished[i]) trigger = 1;
            else if (!node_started[i]) {
                int threshold = n->incoming_count == 0 ? 1 : n->incoming_count;
                if (ctx->node_input_counts[n->index] >= threshold) trigger = 1;
            }
            if (trigger) {
                for (size_t j = 0; j < wf->node_count; j++) {
                    csilk_wf_node_t* p = wf->nodes[j];
                    for (size_t k = 0; k < p->edge_count; k++) if (p->edges[k].target == n && node_finished[j]) { trigger_input = ctx->node_outputs[j]; break; }
                }
                execute_node(ctx, n, trigger_input);
            }
        }
    }
    free(node_started); free(node_finished);
}
