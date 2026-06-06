#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include "csilk/csilk.h"

#ifdef TEST_OOM
#include "csilk/test/test.h"
#endif

#define PORT 8102

static void
dummy_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "OK");
}

static volatile int server_ready = 0;
static csilk_server_t* global_server = nullptr;

static void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* router = csilk_router_new();
	csilk_handler_t h[] = {dummy_handler};
	csilk_router_add(router, "GET", "/", h, 1);

	global_server = csilk_server_new(router);
	// Set a very small connection limit for testing
	csilk_server_set_max_connections(global_server, 1);

	server_ready = 1;
	csilk_server_run(global_server, PORT);

	csilk_server_free(global_server);
	csilk_router_free(router);
	return nullptr;
}

static int
connect_server()
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		return -1;
	}
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(PORT);
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(sock);
		return -1;
	}
	return sock;
}

int
main()
{
	pthread_t thread;
	pthread_create(&thread, nullptr, run_server, nullptr);
	while (!server_ready) {
		usleep(10000);
	}

	printf("Testing max_connections limit...\n");
	// Connect first client and KEEP IT OPEN
	int sock1 = connect_server();
	assert(sock1 >= 0);

	// Give server a bit of time to process the first connection
	usleep(50000);

	// Connect second client - should be accepted and IMMEDIATELY CLOSED by server
	int sock2 = connect_server();
	assert(sock2 >= 0);

	char buf[1024];
	int n = recv(sock2, buf, sizeof(buf), 0);
	// Should get 0 (FIN) or error because server closed it
	assert(n <= 0);

	close(sock2);
	close(sock1);

#ifdef TEST_OOM
	printf("Testing OOM during connection (pool_get failure)...\n");
	// Reset server connection limit to something larger
	csilk_server_set_max_connections(global_server, 10);

	g_oom_fail_after = 0; // Fail next allocation
	g_oom_count = 0;

	int sock3 = connect_server();
	if (sock3 >= 0) {
		struct timeval tv = {0, 500000}; // 500ms timeout
		setsockopt(sock3, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
		n = recv(sock3, buf, sizeof(buf), 0);
		// We don't strictly assert n here because if the server failed to accept,
		// n might be -1 (timeout). The goal is to ensure NO CRASH.
		close(sock3);
	}

	g_oom_fail_after = -1;
#endif

	printf("Stopping server...\n");
	csilk_server_stop(global_server);
	pthread_join(thread, nullptr);

	printf("test_server_limits: PASS\n");
	return 0;
}
