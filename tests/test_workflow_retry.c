#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_attempts = 0;
static int g_done = 0;

/* --- Handlers --- */

csilk_data_t*
flakey_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	g_attempts++;
	printf("[FlakeyNode] Attempt %d\n", g_attempts);

	if (g_attempts < 3) {
		printf("[FlakeyNode] Failing deliberately\n");
		return nullptr;
	}

	printf("[FlakeyNode] Succeeding on attempt %d!\n", g_attempts);
	return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "success"));
}

void
on_retry_complete(csilk_data_t* result)
{
	assert(result != nullptr);
	assert(strcmp((char*)result->value, "success") == 0);
	g_done = 1;
}

void
test_workflow_automatic_retries()
{
	printf("Testing automatic node retries...\n");
	g_attempts = 0;
	g_done = 0;

	csilk_wf_t* wf = csilk_wf_new("retry_wf");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "flakey", flakey_handler, nullptr);
	csilk_wf_node_set_retry(n1, 3, 50); // 3 retries, 50ms delay

	csilk_wf_node_set_entry(n1, 1);

	csilk_wf_run(wf, nullptr, on_retry_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(g_attempts == 3);
	assert(g_done == 1);

	csilk_wf_free(wf);
	printf("test_workflow_automatic_retries: PASS\n");
}

int
main()
{
	test_workflow_automatic_retries();
	return 0;
}
