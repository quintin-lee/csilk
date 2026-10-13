#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static bool
custom_skip(csilk_ctx_t* c)
{
    const char* path = csilk_get_path(c);
    return path && strcmp(path, "/webhook") == 0;
}

void
test_csrf_whitelist_paths_and_prefixes(void)
{
    const char* exact_paths[] = {"/api/public/submit", NULL};
    const char* prefixes[] = {"/api/auth/", "/static/", NULL};

    csilk_csrf_config_t cfg = {
        .exclude_paths = exact_paths,
        .exclude_prefixes = prefixes,
        .skip_fn = custom_skip,
    };

    /* Exact path match: POST without CSRF token allowed */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/api/public/submit");
    csilk_csrf_middleware_ext(ctx, &cfg);
    assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
    csilk_test_ctx_free(ctx);

    /* Prefix match: POST to /api/auth/login without token allowed */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/api/auth/login");
    csilk_csrf_middleware_ext(ctx, &cfg);
    assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
    csilk_test_ctx_free(ctx);

    /* Custom skip_fn match: POST to /webhook allowed */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/webhook");
    csilk_csrf_middleware_ext(ctx, &cfg);
    assert(csilk_get_status(ctx) != CSILK_STATUS_FORBIDDEN);
    csilk_test_ctx_free(ctx);

    /* Non-whitelisted path: POST to /api/users without token blocked with 403 */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/api/users");
    csilk_csrf_middleware_ext(ctx, &cfg);
    assert(csilk_get_status(ctx) == CSILK_STATUS_FORBIDDEN);
    csilk_test_ctx_free(ctx);

    printf("test_csrf_whitelist_paths_and_prefixes passed\n");
}

void
test_jwt_whitelist_and_skip(void)
{
    const char*         prefixes[] = {"/api/auth/", "/health", NULL};
    csilk_jwt_options_t opts = {
        .algorithm = CSILK_JWT_HS256,
        .exclude_prefixes = prefixes,
        .skip_fn = custom_skip,
    };

    /* Whitelisted prefix without Authorization header passes */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/api/auth/login");
    csilk_jwt_middleware_options(ctx, "test-secret", 11, &opts);
    assert(csilk_get_status(ctx) != CSILK_STATUS_UNAUTHORIZED);
    csilk_test_ctx_free(ctx);

    /* Custom skip function without Authorization header passes */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "POST", "/webhook");
    csilk_jwt_middleware_options(ctx, "test-secret", 11, &opts);
    assert(csilk_get_status(ctx) != CSILK_STATUS_UNAUTHORIZED);
    csilk_test_ctx_free(ctx);

    /* Protected path without Authorization header is blocked with 401 */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/api/protected/profile");
    csilk_jwt_middleware_options(ctx, "test-secret", 11, &opts);
    assert(csilk_get_status(ctx) == CSILK_STATUS_UNAUTHORIZED);
    csilk_test_ctx_free(ctx);

    printf("test_jwt_whitelist_and_skip passed\n");
}

int
main(void)
{
    test_csrf_whitelist_paths_and_prefixes();
    test_jwt_whitelist_and_skip();
    printf("test_csrf_whitelist: ALL PASSED\n");
    return 0;
}
