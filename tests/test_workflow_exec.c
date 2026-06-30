#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"

static int g_steps_completed = 0;
static char* g_final_result = nullptr;

csilk_data_t*
step1_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	printf("Executing Step 1: %s\n", (char*)input->value);
	g_steps_completed++;

	return csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, "result from step 1"));
}

csilk_data_t*
step2_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	printf("Executing Step 2: %s\n", (char*)input->value);
	g_steps_completed++;

	return csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, "result from step 2"));
}

void
on_workflow_complete(csilk_data_t* result)
{
	printf("Workflow complete!\n");
	if (result) {
		g_final_result = strdup((char*)result->value);
		printf("Final Result: %s\n", g_final_result);
		// No need to free result->value or result here if they are in the arena,
		// but the ctx is cleaned up after this.
		// Wait, the ctx is cleaned up AFTER the callback in after_worker_cb.
		// So result is valid during the callback.
	}
}

void
test_workflow_exec_sequential()
{
	printf("Testing sequential workflow execution with Arena...\n");
	g_steps_completed = 0;
	if (g_final_result) {
		free(g_final_result);
		g_final_result = nullptr;
	}

	csilk_wf_t* wf = csilk_wf_new("seq_wf");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", step1_handler, nullptr);
	csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", step2_handler, nullptr);

	csilk_wf_bind(n1, n2);

	csilk_data_t initial_input = {"text/plain", "start", nullptr};

	csilk_wf_run(wf, &initial_input, on_workflow_complete);

	csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

	assert(g_steps_completed == 2);
	assert(g_final_result != nullptr);
	assert(strcmp(g_final_result, "result from step 2") == 0);

	csilk_wf_free(wf);
	free(g_final_result);
	g_final_result = nullptr;
	printf("test_workflow_exec_sequential: PASS\n");
}

int
main()
{
	test_workflow_exec_sequential();
	return 0;
}
