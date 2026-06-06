#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "csilk/csilk.h"

#define NUM_PUB_THREADS 4
#define MSGS_PER_THREAD 100
#define TOTAL_EXPECTED (NUM_PUB_THREADS * MSGS_PER_THREAD)

static _Atomic int g_alice_count = 0;
static _Atomic int g_bob_count = 0;
static _Atomic int g_global_count = 0;

static void
alice_sub(csilk_mq_ctx_t* ctx)
{
	(void)ctx;
	g_alice_count++;
}

static void
bob_sub(csilk_mq_ctx_t* ctx)
{
	(void)ctx;
	g_bob_count++;
}

static void
global_mw(csilk_mq_ctx_t* ctx)
{
	g_global_count++;
	csilk_mq_next(ctx);
}

typedef struct {
	csilk_mq_t* mq;
	const char* topic;
	int id;
	int count;
} thread_arg_t;

static void*
publisher_thread(void* arg)
{
	thread_arg_t* ta = (thread_arg_t*)arg;
	char buf[64];
	for (int i = 0; i < ta->count; i++) {
		int n = snprintf(buf, sizeof(buf), "msg_%d_%d", ta->id, i);
		csilk_mq_publish(ta->mq, ta->topic, buf, (size_t)n);
	}
	return nullptr;
}

static void
drain_loop(uv_loop_t* loop, _Atomic int* counter, int target)
{
	for (int i = 0; i < 10000; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
		if (*counter >= target) {
			return;
		}
	}
}

static void
test_mq_concurrent_publish(void)
{
	printf("Testing MQ Concurrent Publish (%d threads x %d msgs)...\n",
	       NUM_PUB_THREADS,
	       MSGS_PER_THREAD);

	uv_loop_t* loop = uv_default_loop();

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);
	csilk_mq_t* mq = csilk_server_get_mq(server);

	g_alice_count = 0;
	g_bob_count = 0;
	g_global_count = 0;

	csilk_mq_use(mq, nullptr, global_mw);
	csilk_mq_subscribe(mq, "alice", alice_sub);
	csilk_mq_subscribe(mq, "bob", bob_sub);

	pthread_t threads[NUM_PUB_THREADS];
	thread_arg_t args[NUM_PUB_THREADS];
	const char* topics[] = {"alice", "alice", "bob", "bob"};

	for (int i = 0; i < NUM_PUB_THREADS; i++) {
		args[i].mq = mq;
		args[i].topic = topics[i];
		args[i].id = i;
		args[i].count = MSGS_PER_THREAD;
		pthread_create(&threads[i], nullptr, publisher_thread, &args[i]);
	}

	for (int i = 0; i < NUM_PUB_THREADS; i++) {
		pthread_join(threads[i], nullptr);
	}

	drain_loop(loop, &g_global_count, TOTAL_EXPECTED);

	int alice_expected = 2 * MSGS_PER_THREAD;

	assert(g_alice_count == alice_expected);
	assert(g_bob_count == alice_expected);
	assert(g_global_count == TOTAL_EXPECTED);

	csilk_server_free(server);
	csilk_router_free(router);
	printf("MQ concurrent publish: PASS (%d alice, %d bob, %d global)\n",
	       (int)g_alice_count,
	       (int)g_bob_count,
	       (int)g_global_count);
}

static _Atomic int g_stress_count = 0;

static void
stress_sub(csilk_mq_ctx_t* ctx)
{
	(void)ctx;
	g_stress_count++;
}

#define STRESS_THREADS 4
#define STRESS_MSGS 250

static void
test_mq_concurrent_stress(void)
{
	printf("Testing MQ Concurrent Stress (%d threads x %d msgs)...\n",
	       STRESS_THREADS,
	       STRESS_MSGS);

	uv_loop_t* loop = uv_default_loop();

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);
	csilk_mq_t* mq = csilk_server_get_mq(server);

	g_stress_count = 0;
	for (int i = 0; i < STRESS_THREADS; i++) {
		char topic_buf[32];
		snprintf(topic_buf, sizeof(topic_buf), "stress.%d", i);
		csilk_mq_subscribe(mq, topic_buf, stress_sub);
	}

	pthread_t threads[STRESS_THREADS];
	thread_arg_t args[STRESS_THREADS];

	for (int i = 0; i < STRESS_THREADS; i++) {
		char topic_buf[32];
		snprintf(topic_buf, sizeof(topic_buf), "stress.%d", i);
		args[i].mq = mq;
		args[i].topic = strdup(topic_buf);
		args[i].id = i;
		args[i].count = STRESS_MSGS;
		pthread_create(&threads[i], nullptr, publisher_thread, &args[i]);
	}

	for (int i = 0; i < STRESS_THREADS; i++) {
		pthread_join(threads[i], nullptr);
		free((char*)args[i].topic);
	}

	drain_loop(loop, &g_stress_count, STRESS_THREADS * STRESS_MSGS);

	int expected = STRESS_THREADS * STRESS_MSGS;
	printf("MQ concurrent stress: %s (%d delivered)\n",
	       g_stress_count == expected ? "PASS" : "FAIL",
	       (int)g_stress_count);
	assert(g_stress_count == expected);

	csilk_server_free(server);
	csilk_router_free(router);
}

static _Atomic int g_rapid_count = 0;

static void
rapid_handler(csilk_mq_ctx_t* ctx)
{
	(void)ctx;
	g_rapid_count++;
}

static void
test_mq_rapid_publish(void)
{
	printf("Testing MQ Rapid Publish (burst from single thread)...\n");

	uv_loop_t* loop = uv_default_loop();

	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);
	csilk_mq_t* mq = csilk_server_get_mq(server);

	g_rapid_count = 0;
	csilk_mq_subscribe(mq, "rapid", rapid_handler);

	for (int i = 0; i < 1000; i++) {
		csilk_mq_publish(mq, "rapid", "data", 4);
	}

	drain_loop(loop, &g_rapid_count, 1000);

	assert(g_rapid_count == 1000);

	csilk_server_free(server);
	csilk_router_free(router);
	printf("MQ rapid publish: PASS (%d delivered)\n", (int)g_rapid_count);
}

int
main(void)
{
	test_mq_concurrent_publish();
	test_mq_concurrent_stress();
	test_mq_rapid_publish();
	printf("test_mq_concurrent: ALL PASSED\n");
	return 0;
}
