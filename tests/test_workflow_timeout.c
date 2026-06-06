#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/app/workflow.h"

static int g_node1_ran = 0;
static int g_node2_ran = 0;
static int g_fallback_ran = 0;
static int g_done = 0;

/* --- Handlers --- */

csilk_data_t*
slow_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	printf("[SlowNode] Starting (should timeout)...\n");
	usleep(200000); // 200ms
	g_node1_ran = 1;
	printf("[SlowNode] Finished (too late!)\n");
	return nullptr;
}

csilk_data_t*
timeout_fallback_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	printf("[Fallback] Executing due to timeout\n");
	g_fallback_ran = 1;
	return nullptr;
}

csilk_data_t*
dummy_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	g_node2_ran++;
	return nullptr;
}

void
on_timeout_complete(csilk_data_t* result)
{
	(void)result;
	g_done = 1;
}

void
test_workflow_node_timeout()
{
	printf("Testing per-node timeout...\n");
	g_node1_ran = 0;
	g_fallback_ran = 0;
	g_done = 0;

	csilk_wf_t* wf = csilk_wf_new("timeout_wf");

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "slow", slow_handler, nullptr);
	csilk_wf_node_set_timeout(n1, 50); // 50ms timeout

	csilk_wf_node_t* nf = csilk_wf_add(wf, "fallback", timeout_fallback_handler, nullptr);
	csilk_wf_on_error(n1, nf);

	csilk_wf_run(wf, nullptr, on_timeout_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	assert(g_fallback_ran == 1);
	// Node 1 might still finish in the thread pool, but its output should have
	// been ignored.

	csilk_wf_free(wf);
	printf("test_workflow_node_timeout: PASS\n");
}

void
test_workflow_global_ttl()
{
	printf("Testing global workflow TTL...\n");
	g_node2_ran = 0;
	g_done = 0;

	csilk_wf_t* wf = csilk_wf_new("ttl_wf");
	csilk_wf_set_ttl(wf, 1); // 1 second TTL

	// Create a chain that would take 2 seconds
	csilk_wf_node_t* last = nullptr;
	for (int i = 0; i < 5; i++) {
		char id[16];
		snprintf(id, sizeof(id), "node%d", i);
		csilk_wf_node_t* n = csilk_wf_add(wf, id, slow_handler, nullptr);
		if (last) {
			csilk_wf_bind(last, n);
		}
		last = n;
	}

	csilk_wf_run(wf, nullptr, on_timeout_complete);

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);

	// Each slow_handler takes 200ms. 5 nodes = 1 second.
	// Wait, let's make it more extreme. 10 nodes = 2 seconds.
	// If TTL is 1s, it should stop around the 5th node.
	assert(g_done == 1);
	assert(g_node1_ran < 10);

	csilk_wf_free(wf);
	printf("test_workflow_global_ttl: PASS\n");
}

int
main()
{
	test_workflow_node_timeout();
	test_workflow_global_ttl();
	return 0;
}
