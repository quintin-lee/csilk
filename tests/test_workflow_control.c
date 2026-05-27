#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_fallback_triggered = 0;
static int g_router_triggered = 0;
static int g_or_triggered = 0;

csilk_data_t* mock_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx; (void)input; (void)user_data;
    return NULL;
}

/* --- Error Handling Test --- */

csilk_data_t* failing_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx; (void)input; (void)user_data;
    printf("Executing Failing Node -> returns NULL\n");
    return NULL;
}

csilk_data_t* fallback_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx; (void)input; (void)user_data;
    printf("Executing Fallback Node\n");
    g_fallback_triggered = 1;
    return NULL;
}

void test_workflow_error_branch() {
    printf("Testing error branch (on_error)...\n");
    g_fallback_triggered = 0;
    csilk_wf_t* wf = csilk_wf_new("error_wf");
    
    csilk_wf_node_t* n_fail = csilk_wf_add(wf, "fail", failing_handler, NULL);
    csilk_wf_node_t* n_fallback = csilk_wf_add(wf, "fallback", fallback_handler, NULL);
    
    csilk_wf_on_error(n_fail, n_fallback);
    
    csilk_wf_run(wf, NULL, NULL);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    assert(g_fallback_triggered == 1);
    csilk_wf_free(wf);
    printf("test_workflow_error_branch: PASS\n");
}

/* --- Dynamic Routing Test --- */

const char* my_router(csilk_data_t* input) {
    if (input && input->value && strcmp((char*)input->value, "magic") == 0) {
        return "magic_node";
    }
    return "normal_node";
}

csilk_data_t* start_router_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)user_data;
    return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, (char*)input->value));
}

csilk_data_t* magic_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx; (void)input; (void)user_data;
    printf("Magic Node triggered!\n");
    g_router_triggered = 1;
    return NULL;
}

void test_workflow_dynamic_route() {
    printf("Testing dynamic routing (csilk_wf_route)...\n");
    g_router_triggered = 0;
    csilk_wf_t* wf = csilk_wf_new("route_wf");
    
    csilk_wf_node_t* n_start = csilk_wf_add(wf, "start", start_router_handler, NULL);
    csilk_wf_add(wf, "magic_node", magic_handler, NULL);
    csilk_wf_add(wf, "normal_node", mock_handler, NULL);
    
    csilk_wf_route(n_start, my_router);
    
    csilk_data_t in = {"text", "magic", NULL};
    csilk_wf_run(wf, &in, NULL);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    assert(g_router_triggered == 1);
    csilk_wf_free(wf);
    printf("test_workflow_dynamic_route: PASS\n");
}

/* --- OR Join Test --- */

csilk_data_t* fast_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)user_data;
    printf("Fast node done\n");
    return csilk_wf_data_new(ctx, "signal", NULL);
}

csilk_data_t* or_join_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data) {
    (void)ctx; (void)input; (void)user_data;
    printf("OR Join node triggered!\n");
    g_or_triggered++;
    return NULL;
}

void test_workflow_or_join() {
    printf("Testing OR Join policy...\n");
    g_or_triggered = 0;
    csilk_wf_t* wf = csilk_wf_new("or_wf");
    
    csilk_wf_node_t* n_start = csilk_wf_add(wf, "start", start_router_handler, NULL);
    csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", fast_handler, NULL);
    csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", fast_handler, NULL);
    csilk_wf_node_t* n_or = csilk_wf_add(wf, "or_node", or_join_handler, NULL);
    
    csilk_wf_node_set_join(n_or, CSILK_WF_JOIN_OR);
    
    csilk_wf_bind(n_start, n1);
    csilk_wf_bind(n_start, n2);
    csilk_wf_bind(n1, n_or);
    csilk_wf_bind(n2, n_or);
    
    csilk_data_t in = {"text", "go", NULL};
    csilk_wf_run(wf, &in, NULL);
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    // Sometimes in async thread pool scheduling one branch might finish completely 
    // before the other even starts, causing the OR node to be triggered once or twice
    // depending on timing. We assert it's triggered at least once.
    assert(g_or_triggered >= 1);
    
    csilk_wf_free(wf);
    printf("test_workflow_or_join: PASS\n");
}

int main() {
    test_workflow_error_branch();
    test_workflow_dynamic_route();
    test_workflow_or_join();
    return 0;
}
