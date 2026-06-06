/**
 * @file test_https.c
 * @brief Integration tests for HTTPS/TLS functionality.
 * @copyright MIT License
 */

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

#define PORT 8443
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
	printf("  [SERVER] Handling hello request\n");
	csilk_string(c, CSILK_STATUS_OK, "Hello Secure World!");
}

/* ---- Server thread ---- */
static volatile int server_ready = 0;

static void*
run_server(void* arg)
{
	(void)arg;
	printf("  [SERVER] Starting HTTPS server on port %d...\n", PORT);

	csilk_log_config_t log_cfg = {CSILK_LOG_DEBUG, nullptr};
	csilk_log_init(log_cfg); // Enable debug logging

	csilk_router_t* router = csilk_router_new();
	csilk_handler_t h[] = {hello_handler};
	csilk_router_add(router, "GET", "/", h, 1);

	csilk_server_t* server = csilk_server_new(router);

	csilk_server_config_t config;
	memset(&config, 0, sizeof(config));
	config.idle_timeout_ms = 5000;
	config.max_header_size = 8192;
	config.max_body_size = 1024 * 1024;
	config.listen_backlog = 128;
	config.enable_tls = 1;

	if (access("tests/test_cert.pem", F_OK) == 0) {
		config.tls_cert_file = "tests/test_cert.pem";
		config.tls_key_file = "tests/test_key.pem";
	} else if (access("../tests/test_cert.pem", F_OK) == 0) {
		config.tls_cert_file = "../tests/test_cert.pem";
		config.tls_key_file = "../tests/test_key.pem";
	} else {
		fprintf(stderr, "  [SERVER] Certificates not found!\n");
		server_ready = -1;
		return nullptr;
	}

	csilk_server_set_config(server, &config);

	server_ready = 1;
	if (csilk_server_run(server, PORT) < 0) {
		fprintf(stderr, "  [SERVER] Failed to run server\n");
	}

	printf("  [SERVER] Stopping HTTPS server...\n");
	csilk_server_free(server);
	csilk_router_free(router);
	return nullptr;
}

/* ---- HTTPS Client ---- */
static void
test_https_get()
{
	printf("  [CLIENT] Initializing TLS client...\n");
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();

	const SSL_METHOD* method = TLS_client_method();
	SSL_CTX* ctx = SSL_CTX_new(method);
	if (!ctx) {
		test_result("HTTPS GET (SSL_CTX_new)", 0);
		return;
	}

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	printf("  [CLIENT] Connecting to 127.0.0.1:%d...\n", PORT);
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		perror("  [CLIENT] Connect failed");
		test_result("HTTPS GET (connect)", 0);
		SSL_CTX_free(ctx);
		return;
	}

	SSL* ssl = SSL_new(ctx);
	SSL_set_fd(ssl, sock);

	printf("  [CLIENT] Performing TLS handshake...\n");
	if (SSL_connect(ssl) <= 0) {
		ERR_print_errors_fp(stderr);
		test_result("HTTPS GET (SSL_connect)", 0);
	} else {
		test_result("HTTPS TLS Handshake", 1);

		const char* req = "GET / HTTP/1.1\r\nHost: "
				  "localhost\r\nConnection: close\r\n\r\n";
		printf("  [CLIENT] Sending encrypted request...\n");
		SSL_write(ssl, req, (int)strlen(req));

		char buf[BUFSIZE];
		printf("  [CLIENT] Waiting for encrypted response...\n");
		int n = SSL_read(ssl, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			printf("  [CLIENT] Received %d bytes\n", n);
			test_result("HTTPS GET (response received)", 1);
			test_result("HTTPS GET (status 200)",
				    strstr(buf, "HTTP/1.1 200 OK") != nullptr);
			test_result("HTTPS GET (body content)",
				    strstr(buf, "Hello Secure World!") != nullptr);
		} else {
			int err = SSL_get_error(ssl, n);
			fprintf(stderr, "  [CLIENT] SSL_read error: %d\n", err);
			test_result("HTTPS GET (read response)", 0);
		}
		SSL_shutdown(ssl);
	}

	SSL_free(ssl);
	close(sock);
	SSL_CTX_free(ctx);
}

static void
test_https_keepalive()
{
	printf("  [CLIENT] Starting HTTPS keep-alive test...\n");
	const SSL_METHOD* method = TLS_client_method();
	SSL_CTX* ctx = SSL_CTX_new(method);

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		test_result("HTTPS Keep-Alive (connect)", 0);
		SSL_CTX_free(ctx);
		return;
	}

	SSL* ssl = SSL_new(ctx);
	SSL_set_fd(ssl, sock);

	if (SSL_connect(ssl) > 0) {
		const char* req1 = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: "
				   "keep-alive\r\n\r\n";
		SSL_write(ssl, req1, (int)strlen(req1));
		char buf[BUFSIZE];
		int n = SSL_read(ssl, buf, sizeof(buf) - 1);
		test_result("HTTPS Keep-Alive (Req 1)", n > 0 && strstr(buf, "200 OK"));

		const char* req2 = "GET / HTTP/1.1\r\nHost: "
				   "localhost\r\nConnection: close\r\n\r\n";
		SSL_write(ssl, req2, (int)strlen(req2));
		n = SSL_read(ssl, buf, sizeof(buf) - 1);
		test_result("HTTPS Keep-Alive (Req 2)", n > 0 && strstr(buf, "200 OK"));

		SSL_shutdown(ssl);
	} else {
		test_result("HTTPS Keep-Alive (Handshake failed)", 0);
	}

	SSL_free(ssl);
	close(sock);
	SSL_CTX_free(ctx);
}

int
main()
{
	printf("=== HTTPS Integration Tests ===\n\n");
	signal(SIGPIPE, SIG_IGN);

	/* Ensure certs exist (check both current and parent dir) */
	if (access("tests/test_cert.pem", F_OK) == -1 &&
	    access("../tests/test_cert.pem", F_OK) == -1) {
		fprintf(stderr,
			"Error: Test certificates missing. Run openssl command "
			"to generate "
			"them first.\n");
		return 1;
	}

	pthread_t thread;
	pthread_create(&thread, nullptr, run_server, nullptr);
	while (!server_ready) {
		usleep(100000);
	}
	if (server_ready < 0) {
		return 1;
	}
	sleep(1); /* Wait a bit more for server to bind */

	test_https_get();
	test_https_keepalive();

	printf("\n=== Results: %d passed, %d failed ===\n", g_tests_passed, g_tests_failed);

	return g_tests_failed > 0 ? 1 : 0;
}
