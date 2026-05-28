#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

void
test_mq_persistence_api()
{
	printf("Testing MQ Persistence API declaration...\n");
	uv_loop_t* loop = uv_default_loop();
	csilk_server_t* server = csilk_server_new(csilk_router_new());
	csilk_mq_t* mq = csilk_server_get_mq(server);

	/* Test with NULL mq */
	int rc = csilk_mq_set_persistence(NULL, "test.wal");
	assert(rc != 0);

	/* Test with valid mq */
	rc = csilk_mq_set_persistence(mq, "test.wal");
	assert(rc == 0);

	csilk_server_free(server);
	printf("test_mq_persistence_api: PASS\n");
}

int
main()
{
	test_mq_persistence_api();
	return 0;
}
