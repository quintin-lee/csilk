/**
 * @file workflow.c
 * @brief AI Workflow engine implementation.
 */

#include "csilk/app/workflow.h"
#include <stdlib.h>
#include <string.h>

struct csilk_wf_node_s {
  char* id;
  csilk_wf_handler_t handler;
  void* user_data;
  
  /* Connections will be added in Task 2 */
};

struct csilk_wf_s {
  char* name;
  csilk_wf_node_t** nodes;
  size_t node_count;
  size_t node_capacity;
};

csilk_wf_t* csilk_wf_new(const char* name) {
  csilk_wf_t* wf = calloc(1, sizeof(csilk_wf_t));
  if (wf) {
    wf->name = strdup(name);
  }
  return wf;
}

static void node_free(csilk_wf_node_t* node) {
  if (!node) return;
  free(node->id);
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
