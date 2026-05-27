/**
 * @file workflow.c
 * @brief AI Workflow engine implementation.
 */

#include "csilk/app/workflow.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define MAX_WORKFLOW_STEPS 1000

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
};

struct csilk_wf_s {
  char* name;
  csilk_wf_node_t** nodes;
  size_t node_count;
  size_t node_capacity;
  uv_loop_t* loop;
};

struct csilk_wf_ctx_s {
  csilk_wf_t* wf;
  csilk_data_t* initial_input;
  void (*callback)(csilk_data_t*);
  
  int* node_input_counts;    /**< Tracking received inputs per node index. */
  int total_executions;      /**< Safety counter to prevent infinite loops. */
  int nodes_active;          /**< Number of nodes currently running or queued. */
  uv_mutex_t mutex;          /**< Protects scheduler state. */
  
  csilk_arena_t* arena;      /**< Memory arena for this execution. */
  uv_mutex_t arena_mutex;    /**< Protects arena from parallel allocations. */
  
  csilk_data_t** node_outputs; /**< History of outputs per node index. */
};

typedef struct node_work_s {
  uv_work_t req;
  csilk_wf_ctx_t* ctx;
  csilk_wf_node_t* node;
  csilk_data_t* input;
  csilk_data_t* output;
} node_work_t;

csilk_wf_t* csilk_wf_new(const char* name) {
  csilk_wf_t* wf = calloc(1, sizeof(csilk_wf_t));
  if (wf) {
    wf->name = strdup(name);
    wf->loop = uv_default_loop();
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
  // Note: user_data might need freeing if it's the internal AI config
  free(node);
}

void csilk_wf_free(csilk_wf_t* wf) {
  if (!wf) return;
  free(wf->name);
  for (size_t i = 0; i < wf->node_count; i++) {
    node_free(wf->nodes[i]);
  }
  free(wf->nodes);
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
  
  if (!is_loop) {
    to->incoming_count++;
  }
}

void csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to) {
  node_add_edge(from, NULL, to, 0);
}

void csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) {
  node_add_edge(from, condition, to, 0);
}

void csilk_wf_on_loop(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) {
  node_add_edge(from, condition, to, 1);
}

void csilk_wf_on_error(csilk_wf_node_t* from, csilk_wf_node_t* target) {
  if (from) from->error_target = target;
}

void csilk_wf_route(csilk_wf_node_t* node, csilk_wf_router_t router) {
  if (node) node->router_fn = router;
}

void csilk_wf_node_set_join(csilk_wf_node_t* node, csilk_wf_join_policy_t policy) {
  if (node) node->join_policy = policy;
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
  if (news) {
    memcpy(news, s, len + 1);
  }
  return news;
}

csilk_data_t* csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value) {
  csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
  if (data) {
    data->type = csilk_wf_strdup(ctx, type);
    data->value = value;
    data->free_fn = NULL;
  }
  return data;
}

/* --- Template Engine & AI Node --- */

static char* resolve_templates(csilk_wf_ctx_t* ctx, const char* template) {
  if (!template) return NULL;
  
  char* res = csilk_wf_strdup(ctx, template);
  
  for (size_t i = 0; i < ctx->wf->node_count; i++) {
    csilk_wf_node_t* n = ctx->wf->nodes[i];
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "{{%s.value}}", n->id);
    
    char* pos;
    while ((pos = strstr(res, pattern)) != NULL) {
      csilk_data_t* out = ctx->node_outputs[n->index];
      const char* val_str = (out && out->value) ? (const char*)out->value : "(null)";
      
      size_t pat_len = strlen(pattern);
      size_t val_len = strlen(val_str);
      size_t res_len = strlen(res);
      
      char* new_res = csilk_wf_alloc(ctx, res_len - pat_len + val_len + 1);
      size_t prefix_len = pos - res;
      memcpy(new_res, res, prefix_len);
      memcpy(new_res + prefix_len, val_str, val_len);
      strcpy(new_res + prefix_len + val_len, pos + pat_len);
      res = new_res;
    }
  }
  
  char* pos;
  const char* pattern = "{{input.value}}";
  while ((pos = strstr(res, pattern)) != NULL) {
      const char* val_str = (ctx->initial_input && ctx->initial_input->value) ? (const char*)ctx->initial_input->value : "(null)";
      size_t pat_len = strlen(pattern);
      size_t val_len = strlen(val_str);
      size_t res_len = strlen(res);
      char* new_res = csilk_wf_alloc(ctx, res_len - pat_len + val_len + 1);
      size_t prefix_len = pos - res;
      memcpy(new_res, res, prefix_len);
      memcpy(new_res + prefix_len, val_str, val_len);
      strcpy(new_res + prefix_len + val_len, pos + pat_len);
      res = new_res;
  }
  
  return res;
}

static csilk_data_t* ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
  (void)input;
  csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
  
  char* prompt = resolve_templates(ctx, config->prompt);
  printf("[Workflow] AI Node executing with prompt: %s\n", prompt);
  
  const char* api_key = getenv("AGENT_API_KEY");
  if (!api_key) {
      printf("[Workflow] AI Node ERROR: AGENT_API_KEY not set.\n");
      return NULL;
  }

  csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
  if (!ai) return NULL;

  csilk_ai_message_t msgs[2];
  int msg_count = 0;
  if (config->system_msg) {
      msgs[msg_count].role = "system";
      msgs[msg_count].content = config->system_msg;
      msg_count++;
  }
  msgs[msg_count].role = "user";
  msgs[msg_count].content = prompt;
  msg_count++;

  csilk_ai_chat_request_t req = {
    .model = config->model ? config->model : "gpt-3.5-turbo",
    .messages = msgs,
    .message_count = (size_t)msg_count,
    .temperature = config->temperature > 0 ? config->temperature : 0.7,
    .max_tokens = config->max_tokens > 0 ? config->max_tokens : 1024
  };

  csilk_ai_chat_response_t res;
  csilk_data_t* out = NULL;
  if (csilk_ai_chat(ai, &req, &res) == 0) {
      out = csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, res.content));
      csilk_ai_chat_response_free(&res);
  } else {
      printf("[Workflow] AI Node ERROR: chat failed: %s\n", res.error_message ? res.error_message : "unknown");
      csilk_ai_chat_response_free(&res);
  }
  
  csilk_ai_free(ai);
  return out;
}

csilk_wf_node_t* csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config) {
  csilk_ai_config_t* copy = malloc(sizeof(csilk_ai_config_t));
  memcpy(copy, config, sizeof(csilk_ai_config_t));
  copy->model = config->model ? strdup(config->model) : NULL;
  copy->system_msg = config->system_msg ? strdup(config->system_msg) : NULL;
  copy->prompt = config->prompt ? strdup(config->prompt) : NULL;
  
  return csilk_wf_add(wf, id, ai_node_handler, copy);
}

/* --- Visualization --- */

char* csilk_wf_to_mermaid(csilk_wf_t* wf) {
  if (!wf) return NULL;
  
  size_t buf_size = 8192;
  char* buf = malloc(buf_size);
  strcpy(buf, "graph TD\n");
  
  for (size_t i = 0; i < wf->node_count; i++) {
    csilk_wf_node_t* n = wf->nodes[i];
    char line[512];
    
    snprintf(line, sizeof(line), "  %s[%s]\n", n->id, n->id);
    strcat(buf, line);
    
    for (size_t j = 0; j < n->edge_count; j++) {
      csilk_wf_edge_t* e = &n->edges[j];
      if (e->condition) {
          snprintf(line, sizeof(line), "  %s -- %s --> %s\n", n->id, e->condition, e->target->id);
      } else {
          snprintf(line, sizeof(line), "  %s --> %s\n", n->id, e->target->id);
      }
      strcat(buf, line);
    }
    
    if (n->error_target) {
        snprintf(line, sizeof(line), "  %s -. error .-> %s\n", n->id, n->error_target->id);
        strcat(buf, line);
    }
  }
  
  return buf;
}

/* --- Scheduler Implementation --- */

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);

static void cleanup_ctx(csilk_wf_ctx_t* ctx) {
  if (!ctx) return;
  uv_mutex_destroy(&ctx->mutex);
  uv_mutex_destroy(&ctx->arena_mutex);
  csilk_arena_free(ctx->arena);
  free(ctx->node_input_counts);
  free(ctx->node_outputs);
  free(ctx);
}

static void worker_cb(uv_work_t* req) {
  node_work_t* work = (node_work_t*)req->data;
  work->output = work->node->handler(work->ctx, work->input, work->node->user_data);
}

static void after_worker_cb(uv_work_t* req, int status) {
  (void)status;
  node_work_t* work = (node_work_t*)req->data;
  csilk_wf_ctx_t* ctx = work->ctx;
  csilk_wf_node_t* node = work->node;
  csilk_data_t* output = work->output;

  uv_mutex_lock(&ctx->mutex);
  ctx->node_outputs[node->index] = output;
  uv_mutex_unlock(&ctx->mutex);

  // 1. Error Handling
  if (output == NULL && node->error_target) {
    execute_node(ctx, node->error_target, NULL);
    uv_mutex_lock(&ctx->mutex);
    ctx->nodes_active--;
    uv_mutex_unlock(&ctx->mutex);
    free(work);
    return;
  }

  // 2. Routing
  int triggered_count = 0;
  
  if (node->router_fn) {
    const char* target_id = node->router_fn(output);
    if (target_id) {
      for (size_t i = 0; i < ctx->wf->node_count; i++) {
        if (strcmp(ctx->wf->nodes[i]->id, target_id) == 0) {
          execute_node(ctx, ctx->wf->nodes[i], output);
          triggered_count++;
          break;
        }
      }
    }
  } else {
    for (size_t i = 0; i < node->edge_count; i++) {
      csilk_wf_edge_t* edge = &node->edges[i];
      int match = 0;
      if (edge->condition == NULL) {
        match = 1;
      } else if (output && output->type && strcmp(output->type, edge->condition) == 0) {
        match = 1;
      }
      
      if (match) {
        csilk_wf_node_t* target = edge->target;
        int ready = 0;
        
        uv_mutex_lock(&ctx->mutex);
        if (ctx->total_executions < MAX_WORKFLOW_STEPS) {
          ctx->node_input_counts[target->index]++;
          
          if (target->join_policy == CSILK_WF_JOIN_OR) {
            ready = 1;
            ctx->node_input_counts[target->index] = 0;
          } else {
            int threshold = target->incoming_count == 0 ? 1 : target->incoming_count;
            if (ctx->node_input_counts[target->index] >= threshold) {
              ready = 1;
              ctx->node_input_counts[target->index] = 0;
            }
          }
        }
        uv_mutex_unlock(&ctx->mutex);
        
        if (ready) {
          execute_node(ctx, target, output);
          triggered_count++;
        }
      }
    }
  }
  
  uv_mutex_lock(&ctx->mutex);
  ctx->nodes_active--;
  int current_active = ctx->nodes_active;
  uv_mutex_unlock(&ctx->mutex);

  if (triggered_count == 0 && current_active == 0 && ctx->callback) {
    ctx->callback(output);
    cleanup_ctx(ctx);
  }
  
  free(work);
}

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input) {
  uv_mutex_lock(&ctx->mutex);
  ctx->total_executions++;
  ctx->nodes_active++;
  uv_mutex_unlock(&ctx->mutex);

  node_work_t* work = calloc(1, sizeof(node_work_t));
  work->req.data = work;
  work->ctx = ctx;
  work->node = node;
  work->input = input;
  
  uv_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

void csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input,
                  void (*callback)(csilk_data_t* result)) {
  if (!wf || wf->node_count == 0) {
    if (callback) callback(NULL);
    return;
  }
  
  csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
  ctx->wf = wf;
  ctx->initial_input = input;
  ctx->callback = callback;
  ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
  ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
  ctx->arena = csilk_arena_new(0);
  uv_mutex_init(&ctx->mutex);
  uv_mutex_init(&ctx->arena_mutex);
  
  int started = 0;
  // 1. First check if any nodes are explicitly marked as entries
  for (size_t i = 0; i < wf->node_count; i++) {
    if (wf->nodes[i]->is_entry) {
      execute_node(ctx, wf->nodes[i], input);
      started = 1;
    }
  }

  // 2. If no explicit entries, use 0-incoming nodes as entries
  if (!started) {
    for (size_t i = 0; i < wf->node_count; i++) {
      if (wf->nodes[i]->incoming_count == 0) {
        execute_node(ctx, wf->nodes[i], input);
        started = 1;
      }
    }
  }
  
  if (!started && callback) {
    callback(NULL);
    cleanup_ctx(ctx);
  }
}
