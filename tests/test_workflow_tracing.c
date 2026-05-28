#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_done = 0;
static char* g_trace_json = NULL;

csilk_data_t*
t_step_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)user_data;
	const char* val = input ? (char*)input->value : "start";
	printf("[TraceTest] Node %s running with input: %s\n", (char*)user_data, val);
	return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, (char*)user_data));
}

void
on_trace_complete(csilk_data_t* result, csilk_wf_trace_t* trace)
{
	(void)result;
	printf("[TraceTest] Workflow complete! Generating JSON...\n");

	assert(trace != NULL);
	assert(trace->node_count == 2);
	assert(trace->nodes[0]->start_time > 0);
	assert(trace->nodes[1]->end_time >= trace->nodes[1]->start_time);

	g_trace_json = csilk_wf_trace_to_json(trace);
	printf("Full Trace JSON:\n%s\n", g_trace_json);

	csilk_wf_trace_free(trace);
	g_done = 1;
}

void
test_workflow_tracing()
{
	printf("Testing workflow tracing & JSON export...\n");
	g_done = 0;

	csilk_wf_t* wf = csilk_wf_new("trace_wf");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "node1", t_step_handler, "node1_output");
	csilk_wf_node_t* n2 = csilk_wf_add(wf, "node2", t_step_handler, "node2_output");
	csilk_wf_bind(n1, n2);

	csilk_wf_run_traced(wf, NULL, on_trace_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(g_done == 1);
	assert(g_trace_json != NULL);
	assert(strstr(g_trace_json, "node1") != NULL);
	assert(strstr(g_trace_json, "node2_output") != NULL);

	free(g_trace_json);
	csilk_wf_free(wf);
	printf("test_workflow_tracing: PASS\n");
}

int
main()
{
	test_workflow_tracing();
	return 0;
}
