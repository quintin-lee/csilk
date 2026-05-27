#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"

// Internal structures for verification
typedef struct csilk_wf_edge_s {
  char* condition;
  csilk_wf_node_t* target;
} csilk_wf_edge_t;

struct csilk_wf_node_s {
  char* id;
  int index;
  csilk_wf_handler_t handler;
  void* user_data;
  csilk_wf_edge_t* edges;
  size_t edge_count;
  size_t edge_capacity;
  int incoming_count;
  int is_entry;
};

csilk_data_t* mock_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx;
    (void)input;
    (void)user_data;
    return NULL;
}

void test_workflow_graph() {
    printf("Testing workflow graph connectivity...\n");
    csilk_wf_t* wf = csilk_wf_new("graph_wf");
    
    csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", mock_handler, NULL);
    csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", mock_handler, NULL);
    csilk_wf_node_t* n3 = csilk_wf_add(wf, "n3", mock_handler, NULL);
    
    csilk_wf_bind(n1, n2);
    csilk_wf_on(n2, "fail", n1);
    csilk_wf_on(n2, "pass", n3);
    
    // Verify n1 connections
    assert(n1->edge_count == 1);
    assert(n1->edges[0].condition == NULL);
    assert(n1->edges[0].target == n2);
    
    // Verify n2 connections
    assert(n2->edge_count == 2);
    assert(strcmp(n2->edges[0].condition, "fail") == 0);
    assert(n2->edges[0].target == n1);
    assert(strcmp(n2->edges[1].condition, "pass") == 0);
    assert(n2->edges[1].target == n3);
    
    csilk_wf_free(wf);
    printf("test_workflow_graph: PASS\n");
}

int main() {
    test_workflow_graph();
    return 0;
}
