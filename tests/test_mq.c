#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"

static int global_mw_called = 0;
static int topic_mw_called = 0;
static int sub_called = 0;

void
global_mw(csilk_mq_ctx_t* ctx)
{
	global_mw_called++;
	csilk_mq_next(ctx);
}

void
topic_mw(csilk_mq_ctx_t* ctx)
{
	topic_mw_called++;
	csilk_mq_next(ctx);
}

void
sub_handler(csilk_mq_ctx_t* ctx)
{
	size_t len;
	const char* p = csilk_mq_get_payload(ctx, &len);
	assert(strncmp(p, "hello", len) == 0);
	sub_called++;
}

void
test_mq_flow()
{
	printf("Testing MQ Flow...\n");
	uv_loop_t* loop = uv_default_loop();

	csilk_server_t* server = csilk_server_new(csilk_router_new());
	csilk_mq_t* mq = csilk_server_get_mq(server);

	csilk_mq_use(mq, NULL, global_mw);
	csilk_mq_use(mq, "events", topic_mw);
	csilk_mq_subscribe(mq, "events", sub_handler);

	csilk_mq_publish(mq, "events", "hello", 5);

	/* Run loop briefly to process async event */
	/* Note: uv_run with UV_RUN_NOWAIT might need to be called multiple times
     to ensure the async wakeup and the subsequent processing are both handled.
   */
	for (int i = 0; i < 5; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
	}

	assert(global_mw_called == 1);
	assert(topic_mw_called == 1);
	assert(sub_called == 1);

	csilk_server_free(server);
	printf("test_mq_flow: PASS\n");
}

static int wildcard_sub_called = 0;

void
wildcard_sub_handler(csilk_mq_ctx_t* ctx)
{
	wildcard_sub_called++;
}

void
test_mq_wildcard()
{
	printf("Testing MQ Wildcard Topic Matching...\n");
	uv_loop_t* loop = uv_default_loop();

	csilk_server_t* server = csilk_server_new(csilk_router_new());
	csilk_mq_t* mq = csilk_server_get_mq(server);

	wildcard_sub_called = 0;
	csilk_mq_subscribe(mq, "system.*.error", wildcard_sub_handler);

	/* Should match system.db.error */
	csilk_mq_publish(mq, "system.db.error", "db error", 8);

	/* Should not match system.db.info */
	csilk_mq_publish(mq, "system.db.info", "db info", 7);

	/* Should match system.net.error */
	csilk_mq_publish(mq, "system.net.error", "net error", 9);

	for (int i = 0; i < 10; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
	}

	assert(wildcard_sub_called == 2);

	csilk_server_free(server);
	printf("test_mq_wildcard: PASS\n");
}

static int offload_worker_called = 0;
static char offload_payload[32];

void
my_worker(const char* topic, const void* payload, size_t len)
{
	offload_worker_called++;
	if (payload && len > 0) {
		size_t copy_len = len < 31 ? len : 31;
		memcpy(offload_payload, payload, copy_len);
		offload_payload[copy_len] = '\0';
	}
}

void
offload_sub(csilk_mq_ctx_t* ctx)
{
	csilk_mq_offload(ctx, my_worker);
}

void
test_mq_offload()
{
	printf("Testing MQ Offload (Background Worker)...\n");
	uv_loop_t* loop = uv_default_loop();

	csilk_server_t* server = csilk_server_new(csilk_router_new());
	csilk_mq_t* mq = csilk_server_get_mq(server);

	offload_worker_called = 0;
	memset(offload_payload, 0, sizeof(offload_payload));
	csilk_mq_subscribe(mq, "offload", offload_sub);

	csilk_mq_publish(mq, "offload", "work", 4);

	/* Run loop until worker is called.
     We need to give it some time as it runs in a thread pool. */
	for (int i = 0; i < 50 && offload_worker_called == 0; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
		usleep(10000); /* 10ms */
	}

	assert(offload_worker_called == 1);
	assert(strcmp(offload_payload, "work") == 0);

	csilk_server_free(server);
	printf("test_mq_offload: PASS\n");
}

int
main()
{
	test_mq_flow();
	test_mq_wildcard();
	test_mq_offload();
	return 0;
}
