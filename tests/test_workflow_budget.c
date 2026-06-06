#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk/app/workflow.h"
#include "csilk/drivers/ai.h"

static int g_node2_ran = 0;
static int g_callback_called = 0;

/* --- Mock AI Meta --- */
typedef struct {
	char* model;
	int prompt_tokens;
	int completion_tokens;
} mock_ai_meta_t;

/* --- Mock Handlers --- */
csilk_data_t*
expensive_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	printf("[Test] Node 1 running (Cost: 100 tokens)\n");
	csilk_data_t* out = csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "data1"));

	// Simulate token metadata
	mock_ai_meta_t* meta = csilk_wf_alloc(ctx, sizeof(mock_ai_meta_t));
	meta->model = "gpt-expensive";
	meta->prompt_tokens = 50;
	meta->completion_tokens = 50;
	out->meta = meta;

	return out;
}

csilk_data_t*
skip_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	printf("[Test] Node 2 running (Error: Should have been skipped!)\n");
	g_node2_ran = 1;
	return nullptr;
}

void
on_budget_complete(csilk_data_t* result)
{
	printf("[Test] Workflow finished. Result was: %s\n",
	       result ? "present" : "nullptr (expected)");
	assert(result == nullptr); // Should be nullptr because it was terminated
	g_callback_called = 1;
}

void
test_workflow_budget_enforcement()
{
	printf("Testing workflow budget enforcement...\n");
	g_node2_ran = 0;
	g_callback_called = 0;

	csilk_wf_t* wf = csilk_wf_new("budget_wf");

	// Set a small budget
	csilk_wf_set_budget(wf, 50);

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "node1", expensive_handler, nullptr);
	csilk_wf_node_t* n2 = csilk_wf_add(wf, "node2", skip_handler, nullptr);
	csilk_wf_bind(n1, n2);

	csilk_wf_run(wf, nullptr, on_budget_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	// Node 1 used 100 tokens, budget was 50.
	// Node 2 should NOT have ran.
	assert(g_node2_ran == 0);
	assert(g_callback_called == 1);

	csilk_wf_free(wf);
	printf("test_workflow_budget_enforcement: PASS\n");
}

int
main()
{
	test_workflow_budget_enforcement();
	return 0;
}
