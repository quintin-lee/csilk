#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "csilk/app/workflow.h"

static int g_node1_ran = 0;
static int g_node2_ran = 0;
static int g_done = 0;
static char g_exec_id[37];
static char* g_final_val = nullptr;

csilk_data_t*
normal_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	g_node1_ran++;
	return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "step1_ok"));
}

csilk_data_t*
interactive_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	g_node2_ran++;
	const char* val = input ? (char*)input->value : "no_input";
	printf("[InteractiveNode] Running with input: %s\n", val);
	return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, val));
}

void
on_interactive_done(csilk_data_t* result)
{
	if (result && result->value) {
		g_final_val = strdup((char*)result->value);
	}
	g_done = 1;
}

void
test_workflow_human_in_the_loop()
{
	printf("Testing human-in-the-loop (Interactive Nodes)...\n");
	g_node1_ran = 0;
	g_node2_ran = 0;
	g_done = 0;
	mkdir("test_interactive_wals", 0755);

	csilk_wf_t* wf = csilk_wf_new("interactive_wf");
	csilk_wf_set_persistence(wf, "test_interactive_wals");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "node1", normal_handler, nullptr);
	csilk_wf_node_t* n2 = csilk_wf_add(wf, "node2", interactive_handler, nullptr);
	csilk_wf_node_set_interactive(n2, 1);

	csilk_wf_bind(n1, n2);
	csilk_wf_node_set_entry(n1, 1);

	/* Part 1: Start and wait for pause */
	const char* id = csilk_wf_run(wf, nullptr, on_interactive_done);
	strcpy(g_exec_id, id);

	csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

	// Should have ran node1, but paused at node2
	assert(g_node1_ran == 1);
	assert(g_node2_ran == 0);
	assert(g_done == 0);
	printf("[Test] Workflow paused as expected.\n");

	/* Part 2: Signal continue with human input */
	printf("[Test] Signaling continue for %s...\n", g_exec_id);
	csilk_data_t human_input = {"text", "human_approved_data", nullptr, nullptr};
	csilk_wf_signal_continue(wf, g_exec_id, &human_input, on_interactive_done);

	csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

	assert(g_node2_ran == 1);
	assert(g_done == 1);
	assert(g_final_val != nullptr);
	assert(strcmp(g_final_val, "human_approved_data") == 0);

	csilk_wf_free(wf);
	free(g_final_val);
	printf("test_workflow_human_in_the_loop: PASS\n");
}

int
main()
{
	test_workflow_human_in_the_loop();
	return 0;
}
