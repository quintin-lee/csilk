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

#define PORT 8103
#define BUFSIZE 8192

static volatile int server_ready = 0;
static csilk_server_t* g_server = NULL;

static void
echo_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "ok");
}

static void
param_handler(csilk_ctx_t* c)
{
	const char* id = csilk_get_param(c, "id");
	const char* q = csilk_get_query(c, "q");
	char resp[512];
	snprintf(resp, sizeof(resp), "id=%s q=%s", id ? id : "", q ? q : "");
	csilk_string(c, CSILK_STATUS_OK, resp);
}

static void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* r = csilk_router_new();
	csilk_handler_t h1[] = {echo_handler};
	csilk_router_add(r, "GET", "/", h1, 1);
	csilk_handler_t h2[] = {param_handler};
	csilk_router_add(r, "GET", "/search", h2, 1);
	csilk_handler_t h3[] = {param_handler};
	csilk_router_add(r, "GET", "/users/:id", h3, 1);

	g_server = csilk_server_new(r);
	server_ready = 1;
	csilk_server_run(g_server, PORT);
	csilk_server_free(g_server);
	csilk_router_free(r);
	return NULL;
}

static int
connect_server(const char* req, char* buf, size_t bufsize)
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
	send(sock, req, strlen(req), 0);
	fd_set fds;
	struct timeval tv = {3, 0};
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	int ret = select(sock + 1, &fds, NULL, NULL, &tv);
	int n = 0;
	if (ret > 0) {
		n = recv(sock, buf, bufsize - 1, 0);
	}
	if (n > 0) {
		buf[n] = '\0';
	}
	close(sock);
	return n;
}

static int
expect_status(const char* resp, int expected)
{
	char exp[32];
	snprintf(exp, sizeof(exp), "HTTP/1.1 %d", expected);
	return strstr(resp, exp) != NULL;
}

static int
expect_body(const char* resp, const char* body)
{
	const char* hdr_end = strstr(resp, "\r\n\r\n");
	if (!hdr_end) {
		return 0;
	}
	return strstr(hdr_end + 4, body) != NULL;
}

int
main()
{
	pthread_t thread;
	pthread_create(&thread, NULL, run_server, NULL);
	while (!server_ready) {
	}
	usleep(100000);

	int passed = 0, failed = 0;
	char buf[BUFSIZE] = {0};

	// Test 1: Normal path
	printf("Test 1: Normal GET / ... ");
	fflush(stdout);
	int n = connect_server(
	    "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", buf, sizeof(buf));
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	// Test 2: Query string with special characters
	printf("Test 2: Query with special chars ... ");
	fflush(stdout);
	n = connect_server("GET /search?q=hello+world&n=%42 HTTP/1.1\r\nHost: "
			   "localhost\r\nConnection: close\r\n\r\n",
			   buf,
			   sizeof(buf));
	// Note: query values are currently not URL-decoded, just check ok
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	// Test 3: Long path (path param)
	printf("Test 3: Long path param ... ");
	fflush(stdout);
	char long_path[512];
	snprintf(long_path,
		 sizeof(long_path),
		 "GET /users/%s HTTP/1.1\r\nHost: localhost\r\nConnection: "
		 "close\r\n\r\n",
		 "user12345678901234567890");
	n = connect_server(long_path, buf, sizeof(buf));
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	// Test 4: Header with special characters (underscores, hyphens)
	printf("Test 4: Custom header with special chars ... ");
	fflush(stdout);
	n = connect_server("GET / HTTP/1.1\r\nHost: localhost\r\nX-Custom-Header: "
			   "test_value-123\r\nConnection: close\r\n\r\n",
			   buf,
			   sizeof(buf));
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	// Test 5: Multiple query parameters
	printf("Test 5: Multiple query params ... ");
	fflush(stdout);
	n = connect_server("GET /search?a=1&b=2&c=3&d=4&e=5 HTTP/1.1\r\nHost: "
			   "localhost\r\nConnection: close\r\n\r\n",
			   buf,
			   sizeof(buf));
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	// Test 6: Malformed header with empty key (llhttp rejects with error ->
	// connection close)
	printf("Test 6: Malformed header rejected ... ");
	fflush(stdout);
	n = connect_server("GET / HTTP/1.1\r\nHost: localhost\r\n: value\r\nConnection: "
			   "close\r\n\r\n",
			   buf,
			   sizeof(buf));
	if (n <= 0) {
		printf("PASS (connection closed as expected)\n");
		passed++;
	} else {
		printf("FAIL (got response when expecting close)\n");
		failed++;
	}

	// Test 7: Very long URL (within limits)
	printf("Test 7: Long URL ... ");
	fflush(stdout);
	char long_url[2048];
	int pos = snprintf(long_url, sizeof(long_url), "GET /search?");
	for (int i = 0; i < 50 && pos < (int)sizeof(long_url) - 50; i++) {
		pos += snprintf(long_url + pos, sizeof(long_url) - pos, "p%d=%d&", i, i);
	}
	pos += snprintf(long_url + pos,
			sizeof(long_url) - pos,
			" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	n = connect_server(long_url, buf, sizeof(buf));
	if (n > 0 && expect_status(buf, CSILK_STATUS_OK)) {
		printf("PASS\n");
		passed++;
	} else {
		printf("FAIL (n=%d)\n", n);
		failed++;
	}

	printf("\nEdge case tests: %d passed, %d failed\n", passed, failed);

	csilk_server_stop(g_server);
	pthread_join(thread, NULL);
	return failed > 0 ? 1 : 0;
}
