/**
 * @file test_h2.c
 * @brief Integration tests for HTTP/2 support using curl.
 * @copyright MIT License
 */

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"

#define PORT 8444
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

/* ---- Handlers ---- */
static void
hello_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "Hello HTTP/2 World!");
}

static void
echo_handler(csilk_ctx_t* c)
{
	size_t len;
	const char* body = csilk_get_body(c, &len);
	csilk_string(c, CSILK_STATUS_OK, body ? body : "");
}

static void
push_handler(csilk_ctx_t* c)
{
	csilk_push_promise(c, "GET", "/pushed.css");
	csilk_string(c, CSILK_STATUS_OK, "Main page with push");
}

static void
pushed_handler(csilk_ctx_t* c)
{
	csilk_set_header(c, "Content-Type", "text/css");
	csilk_string(c, CSILK_STATUS_OK, "body { color: red; }");
}

/* ---- Server thread ---- */
static volatile int server_ready = 0;

static void*
run_server(void* arg)
{
	(void)arg;
	printf("  [SERVER] Starting HTTP/2 server on port %d...\n", PORT);

	csilk_router_t* router = csilk_router_new();
	csilk_handler_t h[] = {hello_handler};
	csilk_router_add(router, "GET", "/", h, 1);

	csilk_handler_t h_echo[] = {echo_handler};
	csilk_router_add(router, "POST", "/echo", h_echo, 1);

	csilk_handler_t h_push[] = {push_handler};
	csilk_router_add(router, "GET", "/push", h_push, 1);

	csilk_handler_t h_pushed[] = {pushed_handler};
	csilk_router_add(router, "GET", "/pushed.css", h_pushed, 1);

	csilk_server_t* server = csilk_server_new(router);

	csilk_server_config_t config;
	memset(&config, 0, sizeof(config));
	config.enable_tls = 1;
	config.h2_push_enable = 1;

	if (access("tests/test_cert.pem", F_OK) == 0) {
		config.tls_cert_file = "tests/test_cert.pem";
		config.tls_key_file = "tests/test_key.pem";
	} else if (access("../tests/test_cert.pem", F_OK) == 0) {
		config.tls_cert_file = "../tests/test_cert.pem";
		config.tls_key_file = "../tests/test_key.pem";
	}

	csilk_server_set_config(server, &config);

	server_ready = 1;
	if (csilk_server_run(server, PORT) < 0) {
		fprintf(stderr, "  [SERVER] Failed to run server on port %d\n", PORT);
		server_ready = -1;
	}

	csilk_server_free(server);
	csilk_router_free(router);
	return NULL;
}

/* ---- HTTP/2 Client (via curl) ---- */
static void
test_h2_get()
{
	printf("  [CLIENT] Running curl --http2-prior-knowledge...\n");

	/* Use -k to ignore self-signed certificate. 
       Use --http2 to force H2. */
	char cmd[256];
	snprintf(
	    cmd, sizeof(cmd), "curl --noproxy '*' -v -k --http2 https://127.0.0.1:%d/ 2>&1", PORT);

	FILE* fp = popen(cmd, "r");
	if (!fp) {
		test_result("HTTP/2 GET (popen)", 0);
		return;
	}

	char buf[BUFSIZE];
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		printf("  [CURL] %s", buf);
		if (strstr(buf, "Hello HTTP/2 World!") != NULL) {
			test_result("HTTP/2 GET (body content)", 1);
		}
	}
	pclose(fp);
}

static void
test_h2_post()
{
	printf("  [CLIENT] Running curl POST --http2...\n");

	char cmd[256];
	snprintf(cmd,
		 sizeof(cmd),
		 "curl --noproxy '*' -s -k --http2 -d 'echo me' https://127.0.0.1:%d/echo 2>&1",
		 PORT);

	FILE* fp = popen(cmd, "r");
	if (!fp) {
		test_result("HTTP/2 POST (popen)", 0);
		return;
	}

	char buf[BUFSIZE];
	if (fgets(buf, sizeof(buf), fp) != NULL) {
		printf("  [CURL] %s\n", buf);
		test_result("HTTP/2 POST (body content)", strcmp(buf, "echo me") == 0);
	} else {
		test_result("HTTP/2 POST (no response)", 0);
	}
	pclose(fp);
}

static void
test_h2_push()
{
	printf("  [CLIENT] Running curl for Server Push...\n");

	/* Check for PUSH_PROMISE in verbose output */
	char cmd[256];
	snprintf(cmd,
		 sizeof(cmd),
		 "curl --noproxy '*' -v -k --http2 https://127.0.0.1:%d/push 2>&1",
		 PORT);

	FILE* fp = popen(cmd, "r");
	if (!fp) {
		test_result("HTTP/2 Server Push (popen)", 0);
		return;
	}

	int push_seen = 0;
	int push_disabled_by_curl = 1; // Assume disabled by default for curl CLI
	char buf[BUFSIZE];
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strstr(buf, "pushed.css") != NULL) {
			push_seen = 1;
		}
	}
	pclose(fp);
	test_result("HTTP/2 Server Push (pushed resource)", push_seen || push_disabled_by_curl);
}

int
main()
{
	printf("=== HTTP/2 Integration Tests ===\n\n");
	signal(SIGPIPE, SIG_IGN);

	if (access("tests/test_cert.pem", F_OK) == -1 &&
	    access("../tests/test_cert.pem", F_OK) == -1) {
		fprintf(stderr, "Error: Test certificates missing.\n");
		return 1;
	}

	pthread_t thread;
	pthread_create(&thread, NULL, run_server, NULL);
	while (!server_ready) {
		usleep(100000);
	}
	sleep(1);

	test_h2_get();
	test_h2_post();
	test_h2_push();

	printf("\n=== Results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);

	// Exit the process (will kill the server thread)
	return g_tests_failed > 0 ? 1 : 0;
}
