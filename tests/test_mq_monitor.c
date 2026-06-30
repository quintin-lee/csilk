#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

static void
test_mq_monitoring_logic()
{
	printf("Testing MQ real-time monitoring logic...\n");

	csilk_mq_t* mq = _csilk_mq_new(csilk_io_default_loop());

	// Create a dummy context for the monitor
	csilk_ctx_t* mock_ctx = csilk_test_ctx_new();

	csilk_mq_register_monitor(mq, mock_ctx);

	// Instead of full E2E, we verify no crashes during publish
	csilk_mq_publish(mq, "test.topic", (uint8_t*)"payload", 7);

	// Let the loop process the async send
	csilk_io_run((csilk_io_loop_t*)csilk_io_default_loop(), CSILK_IO_RUN_NOWAIT);

	_csilk_mq_free(mq);
	csilk_test_ctx_free(mock_ctx);
	printf("test_mq_monitoring_logic: PASS\n");
}

int
main()
{
	test_mq_monitoring_logic();
	return 0;
}
