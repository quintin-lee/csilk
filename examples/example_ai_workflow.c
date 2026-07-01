/**
 * @file example_ai_workflow.c
 * @brief Demonstrates a multi-step AI Research Assistant workflow with DX
 * features.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

/* --- Custom Router --- */

const char*
critique_router(csilk_data_t* output)
{
	if (output && output->value) {
		if (strstr((char*)output->value, "REJECT")) {
			return "writer";
		}
		return "formatter";
	}
	return "formatter";
}

/* --- Handlers --- */

csilk_data_t*
brainstorm_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	printf("[Workflow] Brainstorming: %s\n", (char*)input->value);
	return csilk_wf_data_new(
	    ctx, "ideas", csilk_wf_strdup(ctx, "Modular architecture, libuv core, AI workflows."));
}

csilk_data_t*
writer_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	printf("[Workflow] Writing draft based on: %s\n", (char*)input->value);
	return csilk_wf_data_new(
	    ctx, "draft", csilk_wf_strdup(ctx, "Csilk is a high-performance C web framework."));
}

static int g_critique_count = 0;
csilk_data_t*
critic_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	g_critique_count++;
	printf("[Workflow] Critiquing (attempt %d)...\n", g_critique_count);

	if (g_critique_count < 2) {
		printf("[Workflow] Result: REJECT\n");
		return csilk_wf_data_new(
		    ctx, "critique", csilk_wf_strdup(ctx, "REJECT: More detail needed."));
	}
	printf("[Workflow] Result: ACCEPT\n");
	return csilk_wf_data_new(ctx, "critique", csilk_wf_strdup(ctx, "ACCEPT: Excellent."));
}

csilk_data_t*
fallback_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	printf("[Workflow] Fallback: AI node failed (likely missing API key).\n");
	return csilk_wf_data_new(
	    ctx, "text/plain", csilk_wf_strdup(ctx, "Fallback content (offline mode)."));
}

void
on_workflow_done(csilk_data_t* result)
{
	printf("\n--- WORKFLOW COMPLETE ---\n");
	if (result && result->value) {
		printf("Final Type: %s\n", result->type);
		printf("Final Output:\n%s\n", (char*)result->value);
	} else {
		printf("Workflow finished without final result (AI node likely "
		       "failed without "
		       "fallback).\n");
	}
}

int
main()
{
	printf("AI Workflow DX & Control Flow Example\n");
	printf("====================================\n\n");

	csilk_wf_t* wf = csilk_wf_new("AdvancedAssistant");

	/* 1. Define Nodes */
	csilk_wf_node_t* n_brain = csilk_wf_add(wf, "brainstorm", brainstorm_handler, nullptr);
	csilk_wf_node_t* n_write = csilk_wf_add(wf, "writer", writer_handler, nullptr);
	csilk_wf_node_t* n_critic = csilk_wf_add(wf, "critic", critic_handler, nullptr);

	/* Use high-level AI Node with Template Injection for formatting */
	csilk_wf_node_t* n_format = csilk_wf_add_ai(
	    wf,
	    "formatter",
	    &(csilk_ai_config_t){.model = "gpt-4",
				 .prompt =
				     "Format this draft into technical markdown: "
				     "{{writer.value}}. Final critique was: {{critic.value}}"});

	/* Add a fallback for the AI node */
	csilk_wf_node_t* n_fallback = csilk_wf_add(wf, "fallback", fallback_handler, nullptr);
	csilk_wf_on_error(n_format, n_fallback);

	/* 2. Set Entry Node */
	csilk_wf_node_set_entry(n_brain, 1);

	/* 3. Logic & Routing */
	csilk_wf_bind(n_brain, n_write);
	csilk_wf_bind(n_write, n_critic);
	csilk_wf_bind(n_critic, n_format); // static edge to prevent auto-start

	/* Use Functional Routing for the loop and conditional progress */
	csilk_wf_route(n_critic, critique_router);

	/* 4. Visualization */
	char* mermaid = csilk_wf_to_mermaid(wf);
	printf("Workflow Topology (Mermaid):\n%s\n", mermaid);
	free(mermaid);

	/* 5. Execution */
	csilk_data_t in = {"text", "Csilk Framework Overview", nullptr};
	csilk_wf_run(wf, &in, on_workflow_done);

	csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

	csilk_wf_free(wf);
	return 0;
}
