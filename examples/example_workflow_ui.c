/**
 * @file example_workflow_ui.c
 * @brief Demonstrates the real-time AI workflow dashboard.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/app/workflow.h"
#include "csilk/csilk.h"

/* Global workflow for the demo handler */
static csilk_wf_t* g_wf = NULL;

csilk_data_t*
delay_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	(void)user_data;
	printf("[Workflow] Node running...\n");
	// Simulate some work
	for (int i = 0; i < 5; i++) {
		usleep(100000);
	}
	return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "completed"));
}

void
monitor_route_handler(csilk_ctx_t* c)
{
	csilk_ws_handshake(c);
	if (csilk_is_websocket(c)) {
		printf("[Server] Dashboard connected via WebSocket\n");
		csilk_wf_register_monitor(g_wf, c);
	}
}

static void
trigger_wf(uv_timer_t* h)
{
	csilk_wf_t* w = (csilk_wf_t*)h->data;
	printf("[Server] Triggering workflow execution...\n");
	csilk_wf_run(w, NULL, NULL);
}

int
main()
{
	printf("Starting Csilk Workflow UI Example on port 8080...\n");
	printf("Open http://localhost:8080/ui in your browser.\n\n");

	csilk_app_t* app = csilk_app_new(NULL);

	/* 1. Define Workflow */
	g_wf = csilk_wf_new("LiveDashboardDemo");
	csilk_wf_node_t* n1 = csilk_wf_add(g_wf, "start", delay_handler, NULL);
	csilk_wf_node_t* n2 = csilk_wf_add(g_wf, "process", delay_handler, NULL);
	csilk_wf_node_t* n3 = csilk_wf_add(g_wf, "finish", delay_handler, NULL);

	csilk_wf_bind(n1, n2);
	csilk_wf_bind(n2, n3);
	csilk_wf_node_set_entry(n1, 1);

	/* 2. Setup Server with UI and Monitor */
	csilk_wf_serve_ui(app, "/ui");

	// Manual WS route for monitoring
	csilk_app_get(app, "/monitor", monitor_route_handler);

	/* 3. Run Workflow periodically for demo */
	uv_timer_t timer;
	uv_timer_init(uv_default_loop(), &timer);
	timer.data = g_wf;
	uv_timer_start(&timer, trigger_wf, 1000, 10000); // Every 10s

	/* 4. Start Server */
	csilk_app_run(app, 8080);

	return 0;
}
