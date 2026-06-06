#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define TEST_PORT 8111

static csilk_server_t* g_server = nullptr;
static volatile int server_ready = 0;

void
idle_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "ok");
}

void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* router = csilk_router_new();
	csilk_group_t* group = csilk_group_new(router, "/");
	csilk_GET(group, "/idle", idle_handler);

	g_server = csilk_server_new(router);
	csilk_server_config_t cfg = {.idle_timeout_ms = 300,
				     .max_body_size = 1048576,
				     .max_header_size = 65536,
				     .listen_backlog = 128};
	csilk_server_set_config(g_server, &cfg);

	__sync_synchronize();
	server_ready = 1;

	csilk_server_run(g_server, TEST_PORT);

	csilk_server_free(g_server);
	csilk_group_free(group);
	csilk_router_free(router);
	return nullptr;
}

int
main()
{
	pthread_t thread;
	pthread_create(&thread, nullptr, run_server, nullptr);

	// Wait for server to be ready
	int retries = 0;
	while (!server_ready && retries < 10) {
		usleep(50000); // 50ms
		retries++;
	}

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(TEST_PORT);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("connect");
		printf("FAIL: connect\n");
		csilk_server_stop(g_server);
		pthread_join(thread, nullptr);
		return 1;
	}
	const char* req = "GET /idle HTTP/1.1\r\nConnection: close\r\n\r\n";
	send(sock, req, strlen(req), 0);

	// Blocking read for response
	char buf[1024] = {0};
	int n = recv(sock, buf, sizeof(buf) - 1, 0);
	int passed = 1;
	if (n <= 0) {
		printf("FAIL: No response (n=%d)\n", n);
		passed = 0;
	} else {
		buf[n] = '\0';
		printf("  Response: %s", buf);
		if (!strstr(buf, "200 OK")) {
			printf("FAIL: Expected 200\n");
			passed = 0;
		} else {
			printf("  (got 200 OK)\n");
		}
	}

	close(sock);
	csilk_server_stop(g_server);
	pthread_join(thread, nullptr);
	return passed ? 0 : 1;
}
