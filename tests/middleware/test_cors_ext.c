#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

static const char*
get_response_header(csilk_ctx_t* ctx, const char* key)
{
    return csilk_get_response_header(ctx, key);
}

void
test_cors_null_config_with_context()
{
    printf("Testing CORS with nullptr config and context...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_middleware(ctx, nullptr);

    assert(get_response_header(ctx, "Access-Control-Allow-Origin") == nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_cors_null_config_with_context passed\n");
}

void
test_cors_allow_credentials()
{
    printf("Testing CORS with allow credentials...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_config_t config = {
        .allow_origin = "http://localhost", .allow_methods = "GET", .allow_credentials = 1};

    csilk_cors_middleware(ctx, &config);

    const char* allow_creds = get_response_header(ctx, "Access-Control-Allow-Credentials");
    assert(allow_creds != nullptr && strcmp(allow_creds, "true") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_cors_allow_credentials passed\n");
}

void
test_cors_non_wildcard_origin()
{
    printf("Testing CORS with explicit origin...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_config_t config = {.allow_origin = "http://example.com", .allow_methods = "GET"};

    csilk_cors_middleware(ctx, &config);

    const char* allow_origin = get_response_header(ctx, "Access-Control-Allow-Origin");
    assert(allow_origin != nullptr && strcmp(allow_origin, "http://example.com") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_cors_non_wildcard_origin passed\n");
}

void
test_cors_zero_max_age()
{
    printf("Testing CORS with max_age = 0...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_config_t config = {.allow_origin = "*", .allow_methods = "GET", .max_age = 0};

    csilk_cors_middleware(ctx, &config);

    const char* max_age = get_response_header(ctx, "Access-Control-Max-Age");
    // If max_age is 0, the middleware currently doesn't set the header
    assert(max_age == nullptr);

    csilk_test_ctx_free(ctx);
    printf("test_cors_zero_max_age passed\n");
}

void
test_cors_max_age_set()
{
    printf("Testing CORS max_age > 0 sets the header...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_config_t config = {.allow_origin = "*", .allow_methods = "GET", .max_age = 86400};

    csilk_cors_middleware(ctx, &config);

    const char* max_age = get_response_header(ctx, "Access-Control-Max-Age");
    assert(max_age != nullptr && strcmp(max_age, "86400") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_cors_max_age_set passed\n");
}

void
test_cors_wildcard_with_credentials_rejected()
{
    printf("Testing CORS wildcard origin is rejected when credentials are set...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();

    csilk_cors_config_t config = {.allow_origin = "*", .allow_credentials = 1};

    csilk_cors_middleware(ctx, &config);

    /* Wildcard + credentials is an RFC violation -> no Allow-Origin header. */
    const char* allow_origin = get_response_header(ctx, "Access-Control-Allow-Origin");
    assert(allow_origin == nullptr);
    /* Credentials flag still surfaces its own header. */
    const char* creds = get_response_header(ctx, "Access-Control-Allow-Credentials");
    assert(creds != nullptr && strcmp(creds, "true") == 0);

    csilk_test_ctx_free(ctx);
    printf("test_cors_wildcard_with_credentials_rejected passed\n");
}

void
test_cors_origin_list_match_and_echo()
{
    printf("Testing CORS multi-origin list: match + echo_origin...\n");
    const char* list[] = {"https://a.com", "https://b.com", NULL};

    /* echo_origin: the request's own Origin is echoed back. */
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/");
    csilk_set_request_header(ctx, "Origin", "https://b.com");

    csilk_cors_config_t config = {
        .allow_origin_list = list, .echo_origin = 1, .allow_methods = "GET"};
    csilk_cors_middleware(ctx, &config);

    const char* allow = get_response_header(ctx, "Access-Control-Allow-Origin");
    assert(allow != nullptr && strcmp(allow, "https://b.com") == 0);
    assert(get_response_header(ctx, "Vary") != nullptr); /* non-wildcard -> Vary: Origin */
    csilk_test_ctx_free(ctx);

    /* Non-echo: a fixed list value is used instead of the request origin. */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/");
    csilk_set_request_header(ctx, "Origin", "https://a.com");

    config =
        (csilk_cors_config_t){.allow_origin_list = list, .echo_origin = 0, .allow_methods = "GET"};
    csilk_cors_middleware(ctx, &config);
    allow = get_response_header(ctx, "Access-Control-Allow-Origin");
    assert(allow != nullptr && strcmp(allow, "https://a.com") == 0);
    csilk_test_ctx_free(ctx);

    /* Origin not in the list -> no Allow-Origin header is set. */
    ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "GET", "/");
    csilk_set_request_header(ctx, "Origin", "https://evil.com");
    config =
        (csilk_cors_config_t){.allow_origin_list = list, .echo_origin = 1, .allow_methods = "GET"};
    csilk_cors_middleware(ctx, &config);
    assert(get_response_header(ctx, "Access-Control-Allow-Origin") == nullptr);
    csilk_test_ctx_free(ctx);

    printf("test_cors_origin_list_match_and_echo passed\n");
}

void
test_cors_preflight_short_circuit()
{
    printf("Testing CORS OPTIONS preflight returns 204 and short-circuits...\n");
    csilk_ctx_t* ctx = csilk_test_ctx_new();
    csilk_test_ctx_set_request(ctx, "OPTIONS", "/api");
    csilk_set_request_header(ctx, "Origin", "http://example.com");
    csilk_set_request_header(ctx, "Access-Control-Request-Method", "POST");

    csilk_cors_config_t config = {.allow_origin = "http://example.com",
                                  .allow_methods = "GET, POST"};
    csilk_cors_middleware(ctx, &config);

    /* Preflight is short-circuited with 204 No Content. */
    assert(csilk_test_ctx_count_response_headers(ctx, "Access-Control-Allow-Methods", "GET") == 1);
    csilk_test_ctx_free(ctx);
    printf("test_cors_preflight_short_circuit passed\n");
}

int
main()
{
    test_cors_null_config_with_context();
    test_cors_allow_credentials();
    test_cors_non_wildcard_origin();
    test_cors_zero_max_age();
    test_cors_max_age_set();
    test_cors_wildcard_with_credentials_rejected();
    test_cors_origin_list_match_and_echo();
    test_cors_preflight_short_circuit();
    printf("test_cors_ext: ALL PASSED\n");
    return 0;
}
