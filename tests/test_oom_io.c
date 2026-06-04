/**
 * @file test_oom_io.c
 * @brief OOM simulation for I/O paths (HTTP/1.1 parsing and response).
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"
#include "core/ctx_internal.h"
#include "core/srv_impl.h"

#ifndef TEST_OOM
#error "TEST_OOM must be defined for this test"
#endif

/* Mock libuv functions or use a real loop if necessary.
 * For this test, we want to exercise the logic in http1.c. */

void
test_oom_http_parser()
{
	printf("Testing HTTP parser OOM...\n");

	/* Setup a mock client and server */
	csilk_server_t* s = csilk_server_new(NULL);
	csilk_client_t client;
	memset(&client, 0, sizeof(csilk_client_t));
	client.server = s;

	/* Initialize libuv handles used in I/O paths */
	uv_loop_t* loop = uv_default_loop();
	uv_timer_init(loop, &client.request_timer);
	uv_timer_init(loop, &client.timer);

	/* Pass NULL as the internal client to prevent _csilk_send_response from 
	 * attempting socket/timer I/O during this pure parsing test. */
	_csilk_ctx_init(&client.ctx, s, NULL);

	llhttp_init(&client.parser, HTTP_REQUEST, &s->settings);
	client.parser.data = &client;

	const char* req =
	    "GET /api/data?key=val HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";

	/* Try to fail at every possible allocation site during parsing */
	for (int i = 0; i < 20; i++) {
		g_oom_fail_after = i;
		g_oom_count = 0;

		/* Reset parser for each run */
		llhttp_init(&client.parser, HTTP_REQUEST, &s->settings);
		client.parser.data = &client;

		enum llhttp_errno err = llhttp_execute(&client.parser, req, strlen(req));

		/* Clean up allocations made during parsing. This must happen
		 * after every iteration — including the last — to prevent
		 * memory leaks (request.body, request.path, response.body,
		 * current_url, header fields, etc.). */
		csilk_ctx_cleanup(&client.ctx);
		if (client.current_url) {
			free(client.current_url);
			client.current_url = NULL;
		}

		if (err != HPE_OK) {
			/* If it failed, it should be due to user error (our callback returning non-zero)
			 * or a parser error. We just care that it doesn't crash. */
		}
	}

	g_oom_fail_after = -1;

	/* Close libuv handles before returning to prevent use-after-return
	 * when subsequent tests run uv_run on the default loop. */
	uv_close((uv_handle_t*)&client.request_timer, NULL);
	uv_close((uv_handle_t*)&client.timer, NULL);
	uv_run(loop, UV_RUN_NOWAIT);

	csilk_server_free(s);
}

void
test_oom_static_file()
{
	printf("Testing Static File OOM...\n");

	/* The static file middleware uses malloc for the file request */
	for (int i = 0; i < 5; i++) {
		csilk_ctx_t* c = csilk_test_ctx_new();
		c->request.path = strdup("/test_oom.c");
		c->request.method = "GET";

		g_oom_fail_after = i;
		g_oom_count = 0;

		csilk_static(c, "tests");

		/* Drain the libuv threadpool before freeing the context,
		 * as csilk_static pushes uv_queue_work requests. */
		uv_run(uv_default_loop(), UV_RUN_DEFAULT);

		g_oom_fail_after = -1;
		csilk_test_ctx_free(c);
	}
}

int
main()
{
	test_oom_http_parser();
	test_oom_static_file();
	printf("All I/O OOM tests completed!\n");
	return 0;
}
