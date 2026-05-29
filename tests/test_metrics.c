/**
 * @file test_metrics.c
 * @brief Test for Prometheus metrics middleware.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
dummy_handler(csilk_ctx_t* c)
{
	(void)c;
}

void
test_metrics()
{
	printf("Testing Metrics middleware...\n");

	csilk_handler_t handlers[] = {
	    (csilk_handler_t)csilk_metrics_middleware, dummy_handler, NULL};
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_test_ctx_set_handlers(c, handlers);

	/* Simulate a request through middleware */
	csilk_next(c);

	/* Call metrics handler to get output */
	csilk_metrics_handler(c);

	/* Verify output */
	assert(csilk_get_status(c) == CSILK_STATUS_OK);
	const char* body = csilk_get_response_body(c, NULL);
	assert(body != NULL);
	assert(strstr(body, "http_requests_total_agg 1") != NULL);
	assert(strstr(body, "http_request_duration_microseconds_agg") != NULL);

	csilk_test_ctx_free(c);

	/* Second request */
	c = csilk_test_ctx_new();
	csilk_test_ctx_set_handlers(c, handlers);
	csilk_next(c);
	csilk_metrics_handler(c);
	body = csilk_get_response_body(c, NULL);
	assert(strstr(body, "http_requests_total_agg 2") != NULL);

	csilk_test_ctx_free(c);
	printf("Metrics test passed!\n");
}

int
main()
{
	test_metrics();
	return 0;
}
