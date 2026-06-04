#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define PORT 8100
#define BUFSIZE 4096

static int g_tests_passed = 0;
static int g_tests_failed = 0;

static void
test_result(const char* name, int ok)
{
	if (ok) {
		printf("  PASS: %s\n", name);
		g_tests_passed++;
	} else {
		printf("  FAIL: %s\n", name);
		g_tests_failed++;
	}
}

static int
recv_response(int sock, char* buf, size_t size)
{
	fd_set fds;
	struct timeval tv = {2, 0};
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	int ret = select(sock + 1, &fds, NULL, NULL, &tv);
	if (ret <= 0) {
		return -1;
	}
	return recv(sock, buf, size - 1, 0);
}

static int
expect_status(const char* resp, int expected)
{
	char expected_str[32];
	snprintf(expected_str, sizeof(expected_str), "HTTP/1.1 %d", expected);
	return strstr(resp, expected_str) != NULL;
}

static int
expect_body(const char* resp, const char* body)
{
	const char* hdr_end = strstr(resp, "\r\n\r\n");
	if (!hdr_end) {
		return 0;
	}
	const char* body_start = hdr_end + 4;
	return strstr(body_start, body) != NULL;
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

static int
send_request(int sock, const char* req)
{
	return send(sock, req, strlen(req), 0);
}

/* ---- Handlers ---- */
static void
stream_handler(csilk_ctx_t* c)
{
	csilk_response_write(c, (uint8_t*)"Hello ", 6);
	csilk_response_write(c, (uint8_t*)"World", 5);
	csilk_response_end(c);
}

static void
ws_handler(csilk_ctx_t* c)
{
	csilk_ws_handshake(c);
}

static void
hello_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "Hello, World!");
}

static void
user_handler(csilk_ctx_t* c)
{
	const char* id = csilk_get_param(c, "id");
	if (id) {
		char resp[256];
		snprintf(resp, sizeof(resp), "User ID: %s", id);
		csilk_string(c, CSILK_STATUS_OK, resp);
	} else {
		csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Missing user ID");
	}
}

static void
login_handler(csilk_ctx_t* c)
{
	cJSON* json = csilk_bind_json(c);
	if (!json) {
		csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Invalid JSON");
		return;
	}
	cJSON* username = cJSON_GetObjectItemCaseSensitive(json, "username");
	cJSON* password = cJSON_GetObjectItemCaseSensitive(json, "password");
	if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
		cJSON_Delete(json);
		csilk_string(c, CSILK_STATUS_BAD_REQUEST, "Username and password required");
		return;
	}
	if (strcmp(username->valuestring, "admin") == 0 &&
	    strcmp(password->valuestring, "pass") == 0) {
		cJSON* resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "token", "test123");
		cJSON_AddStringToObject(resp, "status", "ok");
		csilk_json(c, CSILK_STATUS_OK, resp);
	} else {
		csilk_string(c, CSILK_STATUS_UNAUTHORIZED, "Invalid credentials");
	}
	cJSON_Delete(json);
}

static int
test_validator(const char* token)
{
	return token && strcmp(token, "valid_token") == 0;
}

static void
protected_handler(csilk_ctx_t* c)
{
	const char* token = csilk_get_header(c, "Authorization");
	if (!token || !test_validator(token)) {
		csilk_string(c, CSILK_STATUS_UNAUTHORIZED, "Unauthorized");
		return;
	}
	csilk_string(c, CSILK_STATUS_OK, "Protected data");
}

static void
api_handler(csilk_ctx_t* c)
{
	cJSON* data = cJSON_CreateObject();
	cJSON_AddStringToObject(data, "key", "value");
	cJSON_AddNumberToObject(data, "num", 42);
	csilk_json(c, CSILK_STATUS_OK, data);
}

static void
echo_handler(csilk_ctx_t* c)
{
	const char* name = csilk_get_query(c, "name");
	if (name) {
		char resp[256];
		snprintf(resp, sizeof(resp), "Echo: %s", name);
		csilk_string(c, CSILK_STATUS_OK, resp);
	} else {
		csilk_string(c, CSILK_STATUS_OK, "Echo: none");
	}
}

/* ---- Server thread ---- */
static volatile int server_ready = 0;

static void*
run_server(void* arg)
{
	(void)arg;
	csilk_router_t* router = csilk_router_new();

	csilk_group_t* api = csilk_group_new(router, "/api");
	csilk_GET(api, "/data", api_handler);

	csilk_handler_t h1[] = {hello_handler};
	csilk_router_add(router, "GET", "/", h1, 1);
	csilk_handler_t h2[] = {user_handler};
	csilk_router_add(router, "GET", "/user/:id", h2, 1);
	csilk_handler_t h3[] = {login_handler};
	csilk_router_add(router, "POST", "/login", h3, 1);
	csilk_handler_t h4[] = {protected_handler};
	csilk_router_add(router, "GET", "/protected", h4, 1);
	csilk_handler_t h5[] = {echo_handler};
	csilk_router_add(router, "GET", "/echo", h5, 1);
	csilk_handler_t h6[] = {ws_handler};
	csilk_router_add(router, "GET", "/ws", h6, 1);
	csilk_handler_t h7[] = {stream_handler};
	csilk_router_add(router, "GET", "/stream", h7, 1);

	csilk_server_t* server = csilk_server_new(router);
	server_ready = 1;
	csilk_server_run(server, PORT);

	csilk_server_free(server);
	csilk_group_free(api);
	csilk_router_free(router);
	return NULL;
}

/* ---- Tests ---- */
static void
test_get_root()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET / (connect)", 0);
		return;
	}
	const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET / (response received)", n > 0);
	test_result("GET / (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("GET / (body 'Hello, World!')", expect_body(buf, "Hello, World!"));
}

static void
test_get_user_param()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /user/42 (connect)", 0);
		return;
	}
	const char* req = "GET /user/42 HTTP/1.1\r\nHost: "
			  "localhost\r\nConnection: close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /user/42 (response)", n > 0);
	test_result("GET /user/42 (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("GET /user/42 (body)", expect_body(buf, "User ID: 42"));
}

static void
test_get_api_data()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /api/data (connect)", 0);
		return;
	}
	const char* req = "GET /api/data HTTP/1.1\r\nHost: "
			  "localhost\r\nConnection: close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /api/data (response)", n > 0);
	test_result("GET /api/data (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("GET /api/data (JSON body)", expect_body(buf, "\"key\":\"value\""));
	test_result("GET /api/data (num field)", expect_body(buf, "\"num\":42"));
}

static void
test_post_login_ok()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("POST /login ok (connect)", 0);
		return;
	}
	const char* req = "POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
			  "application/json\r\n"
			  "Content-Length: 38\r\nConnection: close\r\n\r\n"
			  "{\"username\":\"admin\",\"password\":\"pass\"}";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("POST /login ok (response)", n > 0);
	test_result("POST /login ok (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("POST /login ok (token)", expect_body(buf, "\"token\":\"test123\""));
}

static void
test_post_login_bad()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("POST /login bad (connect)", 0);
		return;
	}
	const char* req = "POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
			  "application/json\r\n"
			  "Content-Length: 39\r\nConnection: close\r\n\r\n"
			  "{\"username\":\"admin\",\"password\":\"wrong\"}";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("POST /login bad (response)", n > 0);
	test_result("POST /login bad (status 401)", expect_status(buf, CSILK_STATUS_UNAUTHORIZED));
	test_result("POST /login bad (body)", expect_body(buf, "Invalid credentials"));
}

static void
test_get_protected_ok()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /protected ok (connect)", 0);
		return;
	}
	const char* req = "GET /protected HTTP/1.1\r\nHost: localhost\r\nAuthorization: "
			  "valid_token\r\nConnection: close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /protected ok (response)", n > 0);
	test_result("GET /protected ok (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("GET /protected ok (body)", expect_body(buf, "Protected data"));
}

static void
test_get_protected_unauth()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /protected unauth (connect)", 0);
		return;
	}
	const char* req = "GET /protected HTTP/1.1\r\nHost: "
			  "localhost\r\nConnection: close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /protected unauth (response)", n > 0);
	test_result("GET /protected unauth (status 401)",
		    expect_status(buf, CSILK_STATUS_UNAUTHORIZED));
}

static void
test_get_not_found()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /nonexistent (connect)", 0);
		return;
	}
	const char* req = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: "
			  "close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /nonexistent (response)", n > 0);
	test_result("GET /nonexistent (status 404)", expect_status(buf, CSILK_STATUS_NOT_FOUND));
}

static void
test_get_echo()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /echo (connect)", 0);
		return;
	}
	const char* req = "GET /echo?name=test HTTP/1.1\r\nHost: localhost\r\nConnection: "
			  "close\r\n\r\n";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("GET /echo (response)", n > 0);
	test_result("GET /echo (status 200)", expect_status(buf, CSILK_STATUS_OK));
	test_result("GET /echo (body)", expect_body(buf, "Echo: test"));
}

static void
test_keepalive()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("keep-alive (connect)", 0);
		return;
	}

	const char* req1 = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: "
			   "keep-alive\r\n\r\n";
	const char* req2 = "GET /echo?name=foo HTTP/1.1\r\nHost: localhost\r\nConnection: "
			   "keep-alive\r\n\r\n";
	const char* req3 = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: "
			   "close\r\n\r\n";

	send_request(sock, req1);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	int ok1 = n > 0 && expect_status(buf, CSILK_STATUS_OK) && expect_body(buf, "Hello, World!");

	memset(buf, 0, sizeof(buf));
	send_request(sock, req2);
	n = recv_response(sock, buf, sizeof(buf));
	int ok2 = n > 0 && expect_status(buf, CSILK_STATUS_OK) && expect_body(buf, "Echo: foo");

	memset(buf, 0, sizeof(buf));
	send_request(sock, req3);
	n = recv_response(sock, buf, sizeof(buf));
	int ok3 = n > 0 && expect_status(buf, CSILK_STATUS_NOT_FOUND);

	close(sock);
	test_result("keep-alive req1: GET /", ok1);
	test_result("keep-alive req2: GET /echo?name=foo", ok2);
	test_result("keep-alive req3: GET /nonexistent (404)", ok3);
}

static void
test_post_invalid_json()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("POST /login bad json (connect)", 0);
		return;
	}
	const char* req = "POST /login HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
			  "application/json\r\n"
			  "Content-Length: 11\r\nConnection: close\r\n\r\n"
			  "not-json!!!";
	send_request(sock, req);
	char buf[BUFSIZE] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	close(sock);
	test_result("POST /login bad json (response)", n > 0);
	test_result("POST /login bad json (status 400)",
		    expect_status(buf, CSILK_STATUS_BAD_REQUEST));
}

static void
test_streaming_response()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("GET /stream (connect)", 0);
		return;
	}
	const char* req = "GET /stream HTTP/1.1\r\nHost: "
			  "localhost\r\nConnection: close\r\n\r\n";
	send_request(sock, req);

	/* Read until terminal chunk or EOF */
	char full[8192] = {0};
	int total = 0;
	while (total < (int)sizeof(full) - 1) {
		fd_set fds;
		struct timeval tv = {1, 0};
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		int ret = select(sock + 1, &fds, NULL, NULL, &tv);
		if (ret <= 0) {
			break;
		}
		int n = recv(sock, full + total, (int)sizeof(full) - 1 - total, 0);
		if (n <= 0) {
			break;
		}
		total += n;
		full[total] = '\0';
		if (strstr(full, "0\r\n\r\n")) {
			break;
		}
	}
	close(sock);
	test_result("GET /stream (response received)", total > 0);
	test_result("GET /stream (chunked encoding)",
		    strstr(full, "Transfer-Encoding: chunked") != NULL);
	test_result("GET /stream (chunk 1)", strstr(full, "6\r\nHello ") != NULL);
	test_result("GET /stream (chunk 2)", strstr(full, "5\r\nWorld") != NULL);
	test_result("GET /stream (final chunk)", strstr(full, "0\r\n\r\n") != NULL);
}

static void
test_websocket_handshake()
{
	int sock = connect_server();
	if (sock < 0) {
		test_result("WS handshake (connect)", 0);
		return;
	}
	const char* req = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
			  "Connection: Upgrade\r\n"
			  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
			  "Sec-WebSocket-Version: 13\r\n\r\n";
	send_request(sock, req);
	char buf[4096] = {0};
	int n = recv_response(sock, buf, sizeof(buf));
	test_result("WS handshake (response received)", n > 0);
	test_result("WS handshake (status 101)",
		    expect_status(buf, CSILK_STATUS_SWITCHING_PROTOCOLS));
	test_result("WS handshake (Upgrade: websocket)", strstr(buf, "Upgrade: websocket") != NULL);
	test_result("WS handshake (Sec-WebSocket-Accept)",
		    strstr(buf, "Sec-WebSocket-Accept:") != NULL);
	close(sock);
}

int
main()
{
	printf("=== Integration Tests ===\n\n");

	pthread_t thread;
	pthread_create(&thread, NULL, run_server, NULL);
	while (!server_ready) {
		nanosleep(&(struct timespec){0, 10000000}, NULL);
	}
	nanosleep(&(struct timespec){0, 50000000}, NULL);

	test_get_root();
	test_get_user_param();
	test_get_api_data();
	test_post_login_ok();
	test_post_login_bad();
	test_post_invalid_json();
	test_get_protected_ok();
	test_get_protected_unauth();
	test_get_not_found();
	test_get_echo();
	test_keepalive();
	test_streaming_response();
	test_websocket_handshake();

	printf("\n=== Results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);
	return g_tests_failed > 0 ? 1 : 0;
}
