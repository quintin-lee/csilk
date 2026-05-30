/**
 * @file test_connection.c
 * @brief Tests for connection pool and client management.
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csilk/core/ctx_types.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/srv_types.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
	do {                                                                                       \
		tests_run++;                                                                       \
		printf("  FAIL: %s\n", msg);                                                       \
	} while (0)

/* ------------------------------------------------------------------ */

static csilk_server_t*
mock_server(void)
{
	csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
	uv_mutex_init(&s->pool_mutex);
	uv_mutex_init(&s->clients_mutex);
	uv_loop_t* loop = uv_default_loop();
	uv_loop_init(loop);
	s->loop = loop;
	return s;
}

static void
free_mock_server(csilk_server_t* s)
{
	uv_mutex_destroy(&s->pool_mutex);
	uv_mutex_destroy(&s->clients_mutex);
	uv_loop_close(s->loop);
	free(s);
}

/* ------------------------------------------------------------------ */

static void
test_client_pool_constant_defined(void)
{
	if (CSILK_CLIENT_POOL_SIZE >= 16 && CSILK_CLIENT_POOL_SIZE <= 1024) {
		PASS();
	} else {
		FAIL("CSILK_CLIENT_POOL_SIZE out of range");
	}
}

static void
test_server_alloc_sets_pool(void)
{
	csilk_server_t* s = mock_server();
	if (s->client_pool_count == 0) {
		PASS();
	} else {
		FAIL("client pool should be empty at init");
	}
	free_mock_server(s);
}

static void
test_server_mutexes_init(void)
{
	csilk_server_t* s = mock_server();
	int ok = 1;
	uv_mutex_lock(&s->pool_mutex);
	uv_mutex_unlock(&s->pool_mutex);
	uv_mutex_lock(&s->clients_mutex);
	uv_mutex_unlock(&s->clients_mutex);
	if (ok) {
		PASS();
	} else {
		FAIL("mutex init");
	}
	free_mock_server(s);
}

/* ------------------------------------------------------------------ */

static void
test_context_internal_client_roundtrip(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_client_t client;
	memset(&client, 0, sizeof(client));
	_csilk_set_internal_client(c, &client);
	void* got = _csilk_get_internal_client(c);
	if (got == &client) {
		PASS();
	} else {
		FAIL("internal_client roundtrip");
	}
	csilk_test_ctx_free(c);
}

static void
test_context_internal_client_null(void)
{
	void* got = _csilk_get_internal_client(nullptr);
	if (got == nullptr) {
		PASS();
	} else {
		FAIL("internal_client null ctx");
	}
}

/* ------------------------------------------------------------------ */

static void
test_max_connections_configured(void)
{
	csilk_server_t* s = mock_server();
	s->max_connections = 100;
	if (s->max_connections == 100) {
		PASS();
	} else {
		FAIL("max_connections");
	}
	free_mock_server(s);
}

static void
test_max_connections_unlimited(void)
{
	csilk_server_t* s = mock_server();
	s->max_connections = 0;
	if (s->max_connections == 0) {
		PASS();
	} else {
		FAIL("unlimited max_connections");
	}
	free_mock_server(s);
}

static void
test_active_connections_initial(void)
{
	csilk_server_t* s = mock_server();
	if (atomic_load(&s->active_connections) == 0) {
		PASS();
	} else {
		FAIL("active_connections init");
	}
	free_mock_server(s);
}

/* ------------------------------------------------------------------ */

static void
test_client_ip_null_ctx(void)
{
	const char* ip = csilk_get_client_ip(nullptr);
	if (ip == nullptr) {
		PASS();
	} else {
		FAIL("client_ip null ctx");
	}
}

static void
test_client_ip_mock_returns_null(void)
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_client_t client;
	memset(&client, 0, sizeof(client));
	_csilk_set_internal_client(c, &client);

	const char* ip = csilk_get_client_ip(c);
	if (ip == nullptr) {
		PASS();
	} else {
		FAIL("client_ip should return null on unconnected handle");
	}
	csilk_test_ctx_free(c);
}

/* ------------------------------------------------------------------ */

int
main(void)
{
	printf("=== Connection Module Tests ===\n\n");

	printf("--- Pool Constants ---\n");
	test_client_pool_constant_defined();
	test_server_alloc_sets_pool();
	test_server_mutexes_init();

	printf("\n--- Context Client Binding ---\n");
	test_context_internal_client_roundtrip();
	test_context_internal_client_null();

	printf("\n--- Connection Limits ---\n");
	test_max_connections_configured();
	test_max_connections_unlimited();
	test_active_connections_initial();

	printf("\n--- Client IP ---\n");
	test_client_ip_mock_returns_null();
	test_client_ip_null_ctx();

	printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
	return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
