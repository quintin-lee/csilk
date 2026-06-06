#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define PORT 8097
#define BUFSIZE 8192

static volatile int server_ready = 0;
static csilk_server_t* g_server = nullptr;

static void
hello_handler(csilk_ctx_t* c)
{
	// Large enough to trigger gzip
	char body[2000];
	memset(body, 'A', 1999);
	body[1999] = '\0';
	csilk_string(c, CSILK_STATUS_OK, body);
}

static void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* router = csilk_router_new();
	csilk_handler_t h1[] = {csilk_gzip_middleware, hello_handler};
	csilk_router_add(router, "GET", "/", h1, 2);

	g_server = csilk_server_new(router);
	server_ready = 1;
	csilk_server_run(g_server, PORT);
	csilk_server_free(g_server);
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
	}
	usleep(100000);

	printf("Reproduction: Keep-alive with Gzip...\n");

	int sock = connect_server();
	assert(sock >= 0);

	// Request 1: with Gzip
	const char* req1 = "GET / HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: "
			   "gzip\r\nConnection: keep-alive\r\n\r\n";
	send(sock, req1, strlen(req1), 0);

	char buf1[BUFSIZE] = {0};
	int n1 = recv(sock, buf1, sizeof(buf1), 0);
	assert(n1 > 0);
	assert(strstr(buf1, "Content-Encoding: gzip") != nullptr);
	printf("  Request 1 (Gzip) OK\n");

	// Request 2: without Gzip on same connection
	// If is_async is not reset, this will hang!
	const char* req2 = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	send(sock, req2, strlen(req2), 0);

	printf("  Waiting for Request 2 response (might hang if bug exists)...\n");
	char buf2[BUFSIZE] = {0};

	// Use select with timeout to avoid hanging forever
	fd_set fds;
	struct timeval tv = {2, 0}; // 2 seconds timeout
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);

	if (ret <= 0) {
		printf("  FAIL: Request 2 timed out! (is_async not reset bug "
		       "confirmed)\n");
		close(sock);
		csilk_server_stop(g_server);
		pthread_join(thread, nullptr);
		return 1;
	}

	int n2 = recv(sock, buf2, sizeof(buf2), 0);
	assert(n2 > 0);
	assert(strstr(buf2, "HTTP/1.1 200 OK") != nullptr);
	printf("  Request 2 OK (is_async was reset)\n");

	close(sock);
	csilk_server_stop(g_server);
	pthread_join(thread, nullptr);

	printf("Test passed!\n");
	return 0;
}
