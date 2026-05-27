#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_steps_completed = 0;
static char* g_final_result = NULL;

csilk_data_t* step1_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    printf("Executing Step 1: %s\n", (char*)input->value);
    g_steps_completed++;
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    out->type = strdup("text/plain");
    out->value = strdup("result from step 1");
    return out;
}

csilk_data_t* step2_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    printf("Executing Step 2: %s\n", (char*)input->value);
    g_steps_completed++;
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    out->type = strdup("text/plain");
    out->value = strdup("result from step 2");
    return out;
}

void on_workflow_complete(csilk_data_t* result) {
    printf("Workflow complete!\n");
    if (result) {
        g_final_result = strdup((char*)result->value);
        printf("Final Result: %s\n", g_final_result);
        
        // Cleanup result if we are the owner
        free(result->type);
        free(result->value);
        free(result);
    }
}

void test_workflow_exec_sequential() {
    printf("Testing sequential workflow execution...\n");
    g_steps_completed = 0;
    if (g_final_result) { free(g_final_result); g_final_result = NULL; }
    
    csilk_wf_t* wf = csilk_wf_new("seq_wf");
    
    csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", step1_handler, NULL);
    csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", step2_handler, NULL);
    
    csilk_wf_bind(n1, n2);
    
    csilk_data_t initial_input = {"text/plain", "start", NULL};
    
    csilk_wf_run(wf, &initial_input, on_workflow_complete);
    
    // Run the event loop
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    assert(g_steps_completed == 2);
    assert(g_final_result != NULL);
    assert(strcmp(g_final_result, "result from step 2") == 0);
    
    csilk_wf_free(wf);
    free(g_final_result);
    g_final_result = NULL;
    printf("test_workflow_exec_sequential: PASS\n");
}

int main() {
    test_workflow_exec_sequential();
    return 0;
}
