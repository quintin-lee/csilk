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

#include "csilk/app/admin.h"
#include "csilk/app/app.h"
#include "csilk/core/json/json.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
    } while (0)

static csilk_ctx_t*
make_ctx(void)
{
    csilk_ctx_t* c = csilk_test_ctx_new();
    return c;
}

static void
free_ctx(csilk_ctx_t* c)
{
    csilk_test_ctx_free(c);
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
    int          ok = 1;

    for (int i = 0; i < CSILK_MAX_STORAGE + 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "admin_k%d", i);
        csilk_set(c, key, (void*)(uintptr_t)(i + 1));
    }

    for (int i = 0; i < CSILK_MAX_STORAGE; i++) {
        char key[16];
        snprintf(key, sizeof(key), "admin_k%d", i);
        void* val = csilk_get(c, key);
        if (val == nullptr) {
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

static void
dummy_admin_auth_mw(csilk_ctx_t* c)
{
    csilk_set(c, "admin_authenticated", (void*)(uintptr_t)1);
    csilk_next(c);
}

static void
test_admin_serve_requires_auth(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    /* Unauthenticated csilk_admin_serve must refuse to register routes */
    csilk_admin_serve(app, "/admin");

    csilk_ctx_t* c = make_ctx();
    csilk_test_ctx_set_request(c, "GET", "/admin/stats");
    int matched = csilk_router_match_ctx(csilk_app_router(app), c);
    if (matched == 0) {
        PASS();
    } else {
        FAIL("csilk_admin_serve should not register unauthenticated routes");
    }
    free_ctx(c);
    csilk_app_free(app);
}

static void
test_admin_serve_secure_and_stats_endpoint(void)
{
    csilk_app_t* app = csilk_app_new(nullptr);
    csilk_admin_serve_secure(app, "/admin", dummy_admin_auth_mw);

    csilk_ctx_t* c = make_ctx();
    csilk_test_ctx_set_request(c, "GET", "/admin/stats");
    int matched = csilk_router_match_ctx(csilk_app_router(app), c);
    if (matched == 1) {
        csilk_next(c);
    }
    if (matched == 1 && csilk_get_status(c) == 200 &&
        (int)(uintptr_t)csilk_get(c, "admin_authenticated") == 1) {
        size_t        body_len = 0;
        const char*   body = csilk_get_response_body(c, &body_len);
        csilk_json_t* js = body ? csilk_json_parse_len(body, body_len) : nullptr;
        if (js && csilk_json_get(js, "total_requests") && csilk_json_get(js, "mq") &&
            csilk_json_get(js, "ai")) {
            PASS();
        } else {
            FAIL("admin /stats response missing expected JSON fields");
        }
        if (js) {
            csilk_json_free(js);
        }
    } else {
        FAIL("admin /stats route did not execute or return 200");
    }
    free_ctx(c);
    csilk_app_free(app);
}

int
main(void)
{
    printf("=== Admin Module Tests ===\n\n");

    test_admin_enable_metrics_collection();
    test_admin_stats_collection();
    test_admin_ctx_storage_overflow();
    test_admin_serve_requires_auth();
    test_admin_serve_secure_and_stats_endpoint();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
