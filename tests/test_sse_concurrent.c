#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

#define NUM_SSE_CLIENTS 6

static void
test_sse_multiple_streams(void)
{
	printf("Testing SSE Multiple Concurrent Streams (%d clients)...\n", NUM_SSE_CLIENTS);

	csilk_ctx_t* streams[NUM_SSE_CLIENTS];

	for (int i = 0; i < NUM_SSE_CLIENTS; i++) {
		streams[i] = csilk_test_ctx_new();
		csilk_sse_init(streams[i]);
		assert(csilk_is_sse(streams[i]) == 1);
		assert(csilk_get_status(streams[i]) == CSILK_STATUS_OK);
	}

	for (int i = 0; i < NUM_SSE_CLIENTS; i++) {
		csilk_sse_send(streams[i], "ping", "hello");
	}

	for (int i = 0; i < NUM_SSE_CLIENTS; i++) {
		assert(csilk_is_sse(streams[i]) == 1);
	}

	for (int i = 0; i < NUM_SSE_CLIENTS; i++) {
		csilk_sse_close(streams[i]);
	}

	for (int i = 0; i < NUM_SSE_CLIENTS; i++) {
		csilk_test_ctx_free(streams[i]);
	}

	printf("SSE multiple streams: PASS\n");
}

static void
test_sse_stream_lifecycle(void)
{
	printf("Testing SSE Stream Lifecycle (init → send × N → close → re-init)...\n");

	csilk_ctx_t* c = csilk_test_ctx_new();

	for (int cycle = 0; cycle < 3; cycle++) {
		csilk_sse_send(c, "reset", nullptr);
		csilk_sse_init(c);
		assert(csilk_is_sse(c) == 1);

		for (int j = 0; j < 5; j++) {
			csilk_sse_send(c, "data", "payload");
		}

		csilk_sse_close(c);
		csilk_ctx_cleanup(c);
	}

	csilk_test_ctx_free(c);
	printf("SSE stream lifecycle: PASS\n");
}

static void
test_sse_mixed_events(void)
{
	printf("Testing SSE Mixed Events Across Multiple Streams...\n");

	csilk_ctx_t* streams[4];

	for (int i = 0; i < 4; i++) {
		streams[i] = csilk_test_ctx_new();
		csilk_sse_init(streams[i]);
	}

	csilk_sse_send(streams[0], "event_a", "alpha");
	csilk_sse_send(streams[1], "event_b", "beta");
	csilk_sse_send(streams[0], "event_a", "gamma");
	csilk_sse_send(streams[2], "event_c", "delta");
	csilk_sse_send(streams[3], nullptr, "comment_data");
	csilk_sse_send(streams[1], "event_b", "epsilon");
	csilk_sse_send(streams[0], nullptr, nullptr);

	for (int i = 0; i < 4; i++) {
		assert(csilk_is_sse(streams[i]) == 1);
		csilk_sse_close(streams[i]);
		csilk_test_ctx_free(streams[i]);
	}

	printf("SSE mixed events: PASS\n");
}

int
main(void)
{
	test_sse_multiple_streams();
	test_sse_stream_lifecycle();
	test_sse_mixed_events();

	printf("test_sse_concurrent: ALL PASSED\n");
	return 0;
}
