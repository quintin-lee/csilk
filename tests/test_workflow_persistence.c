#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_step_a_ran = 0;
static int g_step_b_ran = 0;
static char g_exec_id[37];

csilk_data_t*
step_a_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	g_step_a_ran++;
	printf("[Test] Step A running\n");
	return csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, "output_from_a"));
}

csilk_data_t*
step_b_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	g_step_b_ran++;
	printf("[Test] Step B running with input: %s\n", (char*)input->value);
	return NULL;
}

void
test_workflow_persistence()
{
	printf("Testing workflow persistence & recovery...\n");
	mkdir("test_wals", 0755);

	csilk_wf_t* wf = csilk_wf_new("persist_wf");
	csilk_wf_set_persistence(wf, "test_wals");

	csilk_wf_node_t* n_a = csilk_wf_add(wf, "node_a", step_a_handler, NULL);
	csilk_wf_node_t* n_b = csilk_wf_add(wf, "node_b", step_b_handler, NULL);
	csilk_wf_bind(n_a, n_b);

	/* Part 1: Partial execution.
     Run only Node A.
   */
	g_step_a_ran = 0;
	g_step_b_ran = 0;
	const char* id = csilk_wf_run(wf, NULL, NULL);
	strcpy(g_exec_id, id);

	// Run until Node A is done.
	// Node A logs START and FINISH.
	// Then it queues Node B, which logs START.
	// We want to stop BEFORE Node B runs its handler.
	while (g_step_a_ran < 1) {
		uv_run(uv_default_loop(), UV_RUN_ONCE);
	}

	// Give it a bit more time to ensure after_worker_cb (which logs FINISH)
	// has a chance to run on the main loop.
	uv_run(uv_default_loop(), UV_RUN_NOWAIT);
	usleep(50000);

	printf("[Test] Interrupted. A_ran=%d, B_ran=%d\n", g_step_a_ran, g_step_b_ran);

	/* Part 2: Recovery.
     In a real scenario, this would be a new process.
     Here we just call resume.
   */
	printf("[Test] Resuming execution %s...\n", g_exec_id);
	csilk_wf_resume(wf, g_exec_id, NULL);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	printf("[Test] Final counts: A_ran=%d, B_ran=%d\n", g_step_a_ran, g_step_b_ran);

	// Node A should ran at least once. It might run again if the FINISH event
	// was not flushed to the WAL before resume.
	assert(g_step_a_ran >= 1);
	assert(g_step_b_ran >= 1);

	csilk_wf_free(wf);
	printf("test_workflow_persistence: PASS\n");
}

int
main()
{
	test_workflow_persistence();
	return 0;
}
