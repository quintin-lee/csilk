/**
 * @file test_admin.c
 * @brief Tests for the admin dashboard module.
 * @copyright MIT License
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/ctx_types.h"
#include "csilk/core/srv_types.h"
#include "csilk/csilk.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
	do {                                                                                       \
		tests_run++;                                                                       \
		printf("  FAIL: %s\n", msg);                                                       \
	} while (0)

static csilk_ctx_t*
make_ctx(void)
{
	csilk_ctx_t* c = calloc(1, sizeof(csilk_ctx_t));
	c->arena = csilk_arena_new(0);
	return c;
}

static void
free_ctx(csilk_ctx_t* c)
{
	csilk_arena_free(c->arena);
	free(c);
}

static void
test_admin_enable_metrics_collection(void)
{
	csilk_ctx_t* c = make_ctx();
	csilk_set(c, "_admin_metrics_enabled", (void*)(uintptr_t)1);
	void* v = csilk_get(c, "_admin_metrics_enabled");
	if ((int)(uintptr_t)v == 1) {
		PASS();
	} else {
		FAIL("admin metrics flag not set");
	}
	free_ctx(c);
}

static void
test_admin_stats_collection(void)
{
	csilk_ctx_t* c = make_ctx();

	csilk_set(c, "admin_total_requests", (void*)(uintptr_t)42);
	csilk_set(c, "admin_active_connections", (void*)(uintptr_t)7);

	void* reqs = csilk_get(c, "admin_total_requests");
	void* conns = csilk_get(c, "admin_active_connections");

	if ((int)(uintptr_t)reqs == 42 && (int)(uintptr_t)conns == 7) {
		PASS();
	} else {
		FAIL("admin stats values incorrect");
	}
	free_ctx(c);
}

static void
test_admin_ctx_storage_overflow(void)
{
	csilk_ctx_t* c = make_ctx();
	int ok = 1;
	for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
		char key[16];
		snprintf(key, sizeof(key), "admin_k%d", i);
		csilk_set(c, key, (void*)(uintptr_t)i);
		if (csilk_get(c, key) == nullptr) {
			fprintf(stderr, "  DEBUG: failed to set key %s\n", key);
		}
	}
	/* Verify all CSILK_MAX_STORAGE keys are present */
	for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
		char key[16];
		snprintf(key, sizeof(key), "admin_k%d", i);
		void* val = csilk_get(c, key);
		if (val == nullptr) {
			fprintf(stderr, "  DEBUG: lost key %s\n", key);
			ok = 0;
			break;
		}
	}
	if (ok) {
		PASS();
	} else {
		FAIL("storage overflow lost keys");
	}
	free_ctx(c);
}

int
main(void)
{
	printf("=== Admin Module Tests ===\n\n");

	test_admin_enable_metrics_collection();
	test_admin_stats_collection();
	printf("  Calling test_admin_ctx_storage_overflow...\n");
	test_admin_ctx_storage_overflow();

	printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
	return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
