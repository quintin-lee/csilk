/**
 * @file test_oom_io.c
 * @brief OOM simulation for I/O paths (HTTP/1.1 parsing and response).
 *
 * This test exercises the HTTP/1.1 parser and the static-file middleware
 * under controlled out-of-memory conditions.  It uses the injectable
 * allocator from test.h (csilk_test_malloc / csilk_test_realloc) which
 * fails after a configurable number of successful allocations.
 *
 * The goal is NOT to verify correct behaviour under OOM (the code may
 * crash or abort — that is expected) but to ensure it does NOT:
 *   - leak memory (detected by LeakSanitizer)
 *   - access freed stack memory (detected by AddressSanitizer)
 *   - double-free or corrupt the heap
 *
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

/*
 * Guard: this test is compiled only when TEST_OOM is defined (typically
 * via -DENABLE_OOM_TEST=ON in CMake).  Without it the injectable
 * allocator stubs are not linked, so every allocation would use the real
 * malloc and the test would be a no-op.
 */
#ifndef TEST_OOM
#error "TEST_OOM must be defined for this test"
#endif

/* ------------------------------------------------------------------ */
/*  test_oom_http_parser                                               */
/*                                                                     */
/*  Feeds a valid HTTP/1.1 request into llhttp and fails every Nth     */
/*  allocation (N = 0 .. 19).  Each iteration parses the same request  */
/*  string; depending on where the OOM strikes the callbacks may       */
/*  partially allocate resources (URL, headers, body, response).       */
/*                                                                     */
/*  Critical invariant: after every iteration the context and client   */
/*  struct MUST be cleaned up so that subsequent iterations (and the   */
/*  next test function) start from a pristine state.                   */
/* ------------------------------------------------------------------ */

void
test_oom_http_parser()
{
	printf("Testing HTTP parser OOM...\n");

	/*
	 * Create a bare server with no config.  We only need the server
	 * for its llhttp settings (callback table).  No I/O happens.
	 */
	csilk_server_t* s = csilk_server_new(nullptr);

	/*
	 * Stack-allocated fake client.  This is NOT a real connection —
	 * there is no socket, no TLS, no libuv stream handle.  We only
	 * populate enough fields to make the parser callbacks happy:
	 *   - .server           so callbacks can access server config
	 *   - .request_timer    uv_timer_init registers it with the loop
	 *   - .timer            (ditto)
	 *   - .ctx              request context (init below)
	 *   - .parser           llhttp parser instance
	 *
	 * WARNING: Because client lives on the stack we MUST close the
	 * timer handles before returning (see end of function), otherwise
	 * the default loop still references them and a later csilk_io_run()
	 * in another test will hit use-after-return.
	 */
	csilk_client_t client;
	memset(&client, 0, sizeof(csilk_client_t));
	client.server = s;

	uv_loop_t* loop = csilk_io_default_loop();
	uv_timer_init(loop, &client.request_timer);
	uv_timer_init(loop, &client.timer);

	/*
	 * Initialise the request context with _internal_client = nullptr.
	 *
	 * This is deliberate: when _csilk_dispatch_request eventually
	 * calls _csilk_send_response, the latter checks
	 *   if (!client) return;
	 * and bails out immediately, preventing any actual socket I/O.
	 * We only care about the allocation / deallocation paths, not
	 * about sending bytes on the wire.
	 */
	_csilk_ctx_init(&client.ctx, s, nullptr);

	llhttp_init(&client.parser, HTTP_REQUEST, &s->settings);
	client.parser.data = &client;

	/*
	 * A short HTTP/1.1 GET request with a body ("hello").
	 * The body exercises the on_body callback which does a realloc.
	 */
	const char* req =
	    "GET /api/data?key=val HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";

	/*
	 * OOM sweep: 20 iterations, each failing one allocation later.
	 *
	 * g_oom_fail_after  = the allocation index at which to fail
	 * g_oom_count       = global counter incremented by every
	 *                     csilk_test_malloc / csilk_test_realloc call
	 *
	 * Iteration i = 0: fail on the very first allocation.
	 * Iteration i = 1: let the first allocation succeed, fail the second.
	 * ...
	 * Iteration i = 19: let 19 allocations succeed, fail the 20th.
	 *
	 * Because the request string is short and triggers only a handful
	 * of callbacks (on_url, on_header_field × 2, on_header_value × 2,
	 * on_body, on_message_complete), later iterations (i >= ~8) will
	 * parse the entire request successfully — meaning on_message_complete
	 * fires, which in turn calls finalize_request (allocates path via
	 * csilk_split_url, possibly allocates a 404 response body via
	 * csilk_string) and _csilk_dispatch_request.
	 */
	for (int i = 0; i < 20; i++) {
		g_oom_fail_after = i;
		g_oom_count = 0;

		/* Reset the llhttp parser state machine for a fresh parse. */
		llhttp_init(&client.parser, HTTP_REQUEST, &s->settings);
		client.parser.data = &client;

		/*
		 * Run the parser.  Depending on g_oom_fail_after this may:
		 *   - succeed completely (err == HPE_OK)
		 *   - fail partway through (err == HPE_USER) when a
		 *     callback returns non-zero after an allocation fails
		 *   - abort early because llhttp itself runs out of memory
		 */
		enum llhttp_errno err = llhttp_execute(&client.parser, req, strlen(req));

		/*
		 * CRITICAL: clean up after every iteration.
		 *
		 * During llhttp_execute the callbacks may have allocated:
		 *   - client.current_url       (on_url callback)
		 *   - client.current_header_*   (on_header_field / on_header_value)
		 *   - client.ctx.request.body  (on_body — realloc)
		 *   - client.ctx.request.path  (finalize_request → csilk_split_url)
		 *   - client.ctx.response.body (_csilk_dispatch_request → csilk_string)
		 *
		 * csilk_ctx_cleanup frees request.body, request.path, and
		 * response.body (if body_is_managed is set).  It does NOT
		 * free current_url or the header fields — those belong to
		 * the client struct, not the context — so we must free
		 * them manually.
		 *
		 * Placing cleanup AFTER llhttp_execute (rather than before)
		 * is essential: if cleanup ran before, the last iteration
		 * would leak everything because no subsequent iteration
		 * would clean up after it.
		 */
		csilk_ctx_cleanup(&client.ctx);
		/*
		 * current_url is csilk_str_view_t (a {data, len} struct),
		 * NOT a raw pointer.  The .data field is either arena-
		 * allocated (freed by csilk_ctx_cleanup above) or points
		 * into the stack-allocated request string — never malloc'd.
		 * Simply reset the fields.
		 */
		client.current_url.data = nullptr;
		client.current_url.len = 0;

		/*
		 * We do not assert on err.  The parser may fail for many
		 * reasons (OOM in a callback, or llhttp itself hitting an
		 * error state).  The only thing we verify is that the
		 * process does not crash, leak, or corrupt memory.
		 */
		if (err != HPE_OK) {
			/* Expected — allocation failed at some point. */
		}
	}

	g_oom_fail_after = -1;

	/*
	 * Close libuv timer handles before the stack frame goes away.
	 *
	 * uv_timer_init added the handles to the default loop's handle
	 * list.  Even though we never called uv_timer_start, uv_close
	 * still adds them to the close-waiting queue.  If we do not
	 * process that queue now, the next test function (which calls
	 * uv_run) will try to remove the handles from the queue and
	 * write to memory that has already been reclaimed — a textbook
	 * stack-use-after-return ASAN error.
	 */
	uv_close((uv_handle_t*)&client.request_timer, nullptr);
	uv_close((uv_handle_t*)&client.timer, nullptr);
	csilk_io_run(loop, CSILK_IO_RUN_NOWAIT);

	csilk_server_free(s);
}

/* ------------------------------------------------------------------ */
/*  test_oom_static_file                                               */
/*                                                                     */
/*  Exercises the csilk_static() middleware under OOM.  This function  */
/*  reads a file from disk and sends it via libuv's threadpool         */
/*  (uv_queue_work).  Because the file operation is asynchronous we    */
/*  must drain the event loop with uv_run before freeing the context.  */
/* ------------------------------------------------------------------ */

void
test_oom_static_file()
{
	printf("Testing Static File OOM...\n");

	/*
	 * The static file middleware allocates memory for the file path,
	 * the file content buffer, and the response body.  Each iteration
	 * lets one more allocation succeed before failing.
	 */
	for (int i = 0; i < 5; i++) {
		/*
		 * csilk_test_ctx_new allocates a fresh context with an
		 * arena and a clean state.  We set up a minimal request
		 * for a file that exists in the repository.
		 */
		csilk_ctx_t* c = csilk_test_ctx_new();
		c->request.path = strdup("/test_oom.c");
		c->request.method = "GET";

		g_oom_fail_after = i;
		g_oom_count = 0;

		/*
		 * Invoke the static-file middleware.  Under the hood this
		 * calls uv_queue_work to read the file in a background
		 * thread.  If an allocation fails partway through the
		 * middleware may return an error response or leave the
		 * context in a partial state.
		 */
		csilk_static(c, "tests");

		/*
		 * Wait for the threadpool work to complete.  Without this
		 * the file-read callback may fire later, accessing the
		 * context after it has been freed — another UAF bug.
		 *
		 * CSILK_IO_RUN_DEFAULT blocks until all active handles complete.
		 * Because uv_queue_work was the only active request, the
		 * loop returns promptly after the callback runs.
		 */
		csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);

		g_oom_fail_after = -1;
		csilk_test_ctx_free(c);
	}
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/*                                                                     */
/*  Runs the two OOM tests sequentially.  Order matters:               */
/*  test_oom_static_file must NOT run first because it would leave     */
/*  the default loop in a state that triggers false positives in the   */
/*  parser test's cleanup assertions.                                  */
/* ------------------------------------------------------------------ */

int
main()
{
	test_oom_http_parser();
	test_oom_static_file();
	printf("All I/O OOM tests completed!\n");
	return 0;
}
