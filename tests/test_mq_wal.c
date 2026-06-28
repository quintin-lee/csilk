#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"

static int received_count = 0;
static char* received_payloads[3] = {nullptr, nullptr, nullptr};

void
on_msg(csilk_mq_ctx_t* ctx)
{
	size_t len = 0;
	const char* payload = (const char*)csilk_mq_get_payload(ctx, &len);

	if (received_count < 3) {
		received_payloads[received_count] = malloc(len + 1);
		memcpy(received_payloads[received_count], payload, len);
		received_payloads[received_count][len] = '\0';
	}

	received_count++;
	csilk_mq_next(ctx);
}

void
test_mq_wal_persistence_recovery()
{
	printf("Testing MQ WAL Persistence and Recovery (Task 4)...\n");
	const char* wal_path = "test_full.wal";
	unlink(wal_path);

	/* Phase 1: Publish messages and shut down */
	{
		printf("Phase 1: Publishing 3 messages...\n");
		csilk_router_t* router = csilk_router_new();
		csilk_server_t* server = csilk_server_new(router);
		csilk_mq_t* mq = csilk_server_get_mq(server);

		int rc = csilk_mq_set_persistence(mq, wal_path);
		assert(rc == 0);

		rc = csilk_mq_publish(mq, "test.wal", "msg1", 4);
		assert(rc == 0);
		rc = csilk_mq_publish(mq, "test.wal", "msg2", 4);
		assert(rc == 0);
		rc = csilk_mq_publish(mq, "test.wal", "msg3", 4);
		assert(rc == 0);

		/* Wait a bit for WAL to be written if it's async (though usually it's sync
     * in this impl) */
		usleep(100000);

		csilk_server_free(server);
		csilk_router_free(router);

		/* Ensure all libuv handles are closed and loop finishes */
		csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_DEFAULT);
	}

	/* Phase 2: Re-init and verify recovery */
	printf("Phase 2: Re-initializing and verifying recovery...\n");
	received_count = 0;
	{
		csilk_router_t* router = csilk_router_new();
		csilk_server_t* server = csilk_server_new(router);
		csilk_mq_t* mq = csilk_server_get_mq(server);

		/* Subscribe BEFORE setting persistence to catch recovered messages */
		csilk_mq_subscribe(mq, "test.wal", on_msg);

		/* Set persistence to the SAME file - this triggers recovery */
		int rc = csilk_mq_set_persistence(mq, wal_path);
		assert(rc == 0);

		/* Run loop to process recovered messages */
		uv_loop_t* loop = csilk_io_default_loop();

		/* We expect 3 messages. Recovery usually queues them via uv_async or
     * similar. */
		/* Run until no more active handles or requested number of messages received
     */
		int retries = 0;
		while (received_count < 3 && retries < 100) {
			csilk_io_run(loop, CSILK_IO_RUN_NOWAIT);
			usleep(10000);
			retries++;
		}

		if (received_count != 3) {
			printf("Error: received_count is %d, expected 3\n", received_count);
			exit(1);
		}

		assert(strcmp(received_payloads[0], "msg1") == 0);
		assert(strcmp(received_payloads[1], "msg2") == 0);
		assert(strcmp(received_payloads[2], "msg3") == 0);

		printf("Successfully recovered 3 messages: %s, %s, %s\n",
		       received_payloads[0],
		       received_payloads[1],
		       received_payloads[2]);

		/* Clean up allocated payloads */
		for (int i = 0; i < 3; i++) {
			free(received_payloads[i]);
			received_payloads[i] = nullptr;
		}

		csilk_server_free(server);
		csilk_router_free(router);
		csilk_io_run(loop, CSILK_IO_RUN_DEFAULT);
	}

	unlink(wal_path);
	printf("test_mq_wal: PASS\n");
}

int
main()
{
	test_mq_wal_persistence_recovery();
	return 0;
}
