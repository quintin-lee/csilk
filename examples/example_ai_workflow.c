/**
 * @file example_ai_workflow.c
 * @brief Demonstrates a multi-step AI Research Assistant workflow.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/csilk.h"
#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

/* --- Handlers --- */

/**
 * @brief Step 1: Brainstorming/Research.
 */
csilk_data_t* brainstorm_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    const char* topic = (const char*)input->value;
    printf("[Workflow] Brainstorming for topic: %s\n", topic);
    
    /* In a real app, you would call csilk_ai_chat() here. */
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    out->type = strdup("ideas");
    out->value = strdup("1. Modular architecture, 2. libuv core, 3. AI unified interface.");
    return out;
}

/**
 * @brief Step 2: Writing a draft based on ideas.
 */
csilk_data_t* writer_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    printf("[Workflow] Writing draft based on ideas...\n");
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    out->type = strdup("draft");
    out->value = strdup("Csilk is a high-performance web framework for C developers...");
    return out;
}

/**
 * @brief Step 3: Critique the draft.
 */
static int critique_count = 0;
csilk_data_t* critic_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    critique_count++;
    printf("[Workflow] Critiquing draft (attempt %d)...\n", critique_count);
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    if (critique_count < 2) {
        printf("[Workflow] Critique: Needs more technical details. -> fail\n");
        out->type = strdup("fail");
    } else {
        printf("[Workflow] Critique: Looks good! -> pass\n");
        out->type = strdup("pass");
    }
    return out;
}

/**
 * @brief Step 4: Final formatting.
 */
csilk_data_t* formatter_handler(csilk_data_t* input, void* user_data) {
    (void)user_data;
    printf("[Workflow] Formatting final output...\n");
    
    csilk_data_t* out = calloc(1, sizeof(csilk_data_t));
    out->type = strdup("text/markdown");
    out->value = strdup("# Csilk Framework\n\nCsilk is a high-performance web framework...");
    return out;
}

void on_workflow_done(csilk_data_t* result) {
    printf("\n--- WORKFLOW COMPLETE ---\n");
    if (result) {
        printf("Result Type: %s\n", result->type);
        printf("Final Content:\n%s\n", (char*)result->value);
    }
}

int main() {
    printf("Starting AI Research Assistant Workflow...\n\n");
    
    /* 1. Create Workflow */
    csilk_wf_t* wf = csilk_wf_new("ResearchAssistant");
    
    /* 2. Add Nodes */
    csilk_wf_node_t* n_brainstorm = csilk_wf_add(wf, "brainstorm", brainstorm_handler, NULL);
    csilk_wf_node_t* n_writer = csilk_wf_add(wf, "writer", writer_handler, NULL);
    csilk_wf_node_t* n_critic = csilk_wf_add(wf, "critic", critic_handler, NULL);
    csilk_wf_node_t* n_format = csilk_wf_add(wf, "formatter", formatter_handler, NULL);
    
    /* n_brainstorm is the entry node (0 incoming edges) */
    
    /* 3. Connect Nodes */
    csilk_wf_bind(n_brainstorm, n_writer); // brainstorm -> writer
    csilk_wf_bind(n_writer, n_critic);     // writer -> critic
    
    /* 4. Conditional Edges (Agentic Loop) */
    csilk_wf_on_loop(n_critic, "fail", n_writer); // Loop back to writer if critique fails
    csilk_wf_on(n_critic, "pass", n_format); // Proceed to formatter if critique passes
    
    /* 5. Run Workflow */
    csilk_data_t input = {"text/plain", "Technical overview of Csilk framework", NULL};
    csilk_wf_run(wf, &input, on_workflow_done);
    
    /* 6. Run Event Loop */
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    /* 7. Cleanup */
    csilk_wf_free(wf);
    
    return 0;
}
