/**
 * @file workflow.c
 * @brief AI Workflow engine implementation.
 */

#include "csilk/app/workflow.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct csilk_wf_edge_s {
  char* condition;           /**< NULL for default/bind. */
  csilk_wf_node_t* target;   /**< Destination node. */
} csilk_wf_edge_t;

struct csilk_wf_node_s {
  char* id;
  csilk_wf_handler_t handler;
  void* user_data;
  
  csilk_wf_edge_t* edges;
  size_t edge_count;
  size_t edge_capacity;
  
  int incoming_count;        /**< Number of incoming edges. */
};

struct csilk_wf_s {
  char* name;
  csilk_wf_node_t** nodes;
  size_t node_count;
  size_t node_capacity;
  uv_loop_t* loop;
};

typedef struct csilk_wf_ctx_s {
  csilk_wf_t* wf;
  csilk_data_t* initial_input;
  void (*callback)(csilk_data_t*);
  
  int* node_input_counts;    /**< Dynamic array tracking received inputs per node. */
  uv_mutex_t mutex;
} csilk_wf_ctx_t;

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
  node->handler = handler;
  node->user_data = user_data;

  wf->nodes[wf->node_count++] = node;
  return node;
}

static void node_add_edge(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) {
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
  to->incoming_count++;
}

void csilk_wf_bind(csilk_wf_node_t* from, csilk_wf_node_t* to) {
  node_add_edge(from, NULL, to);
}

void csilk_wf_on(csilk_wf_node_t* from, const char* condition, csilk_wf_node_t* to) {
  node_add_edge(from, condition, to);
}

/* --- Scheduler Implementation --- */

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input);

static void worker_cb(uv_work_t* req) {
  node_work_t* work = (node_work_t*)req->data;
  work->output = work->node->handler(work->input, work->node->user_data);
}

static void after_worker_cb(uv_work_t* req, int status) {
  (void)status;
  node_work_t* work = (node_work_t*)req->data;
  csilk_wf_ctx_t* ctx = work->ctx;
  csilk_wf_node_t* node = work->node;
  csilk_data_t* output = work->output;

  // Cleanup input if needed? For now assume caller manages it or we copy.
  
  // Find matching outgoing edges
  int triggered = 0;
  for (size_t i = 0; i < node->edge_count; i++) {
    csilk_wf_edge_t* edge = &node->edges[i];
    
    // Match condition
    // For now, if edge->condition is NULL, it's a default/bind edge.
    // If edge->condition is set, match against output->type.
    int match = 0;
    if (edge->condition == NULL) {
      match = 1;
    } else if (output && output->type && strcmp(output->type, edge->condition) == 0) {
      match = 1;
    }
    
    if (match) {
      execute_node(ctx, edge->target, output);
      triggered = 1;
    }
  }
  
  if (!triggered && ctx->callback) {
    // End of workflow or branch
    // Simple logic: if no successors were triggered, we might be done.
    // In a DAG, we should count total nodes completed.
    // For Task 3, we just call the callback with the final output.
    ctx->callback(output);
    
    // Cleanup context (ideally after all branches finish)
    // uv_mutex_destroy(&ctx->mutex);
    // free(ctx->node_input_counts);
    // free(ctx);
  }
  
  free(work);
}

static void execute_node(csilk_wf_ctx_t* ctx, csilk_wf_node_t* node, csilk_data_t* input) {
  node_work_t* work = calloc(1, sizeof(node_work_t));
  work->req.data = work;
  work->ctx = ctx;
  work->node = node;
  work->input = input;
  
  uv_queue_work(ctx->wf->loop, &work->req, worker_cb, after_worker_cb);
}

void csilk_wf_run(csilk_wf_t* wf, csilk_data_t* input,
                  void (*callback)(csilk_data_t* result)) {
  if (!wf) return;
  
  csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
  ctx->wf = wf;
  ctx->callback = callback;
  ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
  uv_mutex_init(&ctx->mutex);
  
  // Find entry nodes (incoming_count == 0)
  int started = 0;
  for (size_t i = 0; i < wf->node_count; i++) {
    if (wf->nodes[i]->incoming_count == 0) {
      execute_node(ctx, wf->nodes[i], input);
      started = 1;
    }
  }
  
  if (!started && callback) {
    callback(NULL);
    free(ctx->node_input_counts);
    free(ctx);
  }
}
