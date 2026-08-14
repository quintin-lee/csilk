#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_jwt_hs256_roundtrip(void)
{
    printf("Testing JWT HS256 round-trip...\n");

    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "sub", "user123");
    csilk_json_add_string(payload, "role", "admin");
    csilk_json_add_number(payload, "iat", 1700000000);

    const char* secret = "my-secret-key-for-testing";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != NULL);

    csilk_json_t* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != NULL);
    assert(strcmp(csilk_json_get_string(verified, "sub"), "user123") == 0);
    assert(strcmp(csilk_json_get_string(verified, "role"), "admin") == 0);

    csilk_json_free(verified);
    csilk_json_free(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_wrong_secret(void)
{
    printf("Testing JWT wrong secret rejection...\n");

    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "sub", "test");

    char* token = csilk_jwt_generate(c, payload, "correct-secret");
    assert(token != NULL);

    csilk_json_t* verified = csilk_jwt_verify(c, token, "wrong-secret");
    assert(verified == NULL);

    csilk_json_free(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_expired_token(void)
{
    printf("Testing JWT expired token (verify checks signature, not exp)...\n");

    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "sub", "user");
    csilk_json_add_number(payload, "exp", 1000000);

    const char* secret = "test-secret";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != NULL);

    /* csilk_jwt_verify without options checks exp when present */
    csilk_json_t* verified = csilk_jwt_verify(c, token, secret);
    assert(verified == NULL);

    csilk_json_free(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_malformed_token(void)
{
    printf("Testing JWT malformed token rejection...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    csilk_json_t* v1 = csilk_jwt_verify(c, "not-a-jwt", "secret");
    assert(v1 == NULL);

    csilk_json_t* v2 = csilk_jwt_verify(c, "only.two", "secret");
    assert(v2 == NULL);

    csilk_json_t* v3 = csilk_jwt_verify(c, "", "secret");
    assert(v3 == NULL);

    csilk_json_t* v4 = csilk_jwt_verify(c, "a.b.c.d", "secret");
    assert(v4 == NULL);

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_no_exp_claim(void)
{
    printf("Testing JWT without exp claim...\n");

    csilk_ctx_t*  c = csilk_test_ctx_new();
    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "sub", "user");
    csilk_json_add_string(payload, "name", "Test User");

    const char* secret = "secret";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != NULL);

    /* Default verify: accepts token without exp */
    csilk_json_t* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != NULL);
    assert(strcmp(csilk_json_get_string(verified, "sub"), "user") == 0);
    csilk_json_free(verified);

    /* Strict policy verify: rejects token without exp when CSILK_JWT_REQUIRE_EXP is set */
    csilk_jwt_options_t opts = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_REQUIRE_EXP,
        .leeway_sec = 0,
    };
    csilk_json_t* strict_verified =
        csilk_jwt_verify_options(c, token, secret, strlen(secret), &opts);
    assert(strict_verified == NULL);

    csilk_json_free(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_require_exp_policy(void)
{
    printf("Testing JWT CSILK_JWT_REQUIRE_EXP policy...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  secret = "secret";

    /* Token with future exp */
    csilk_json_t* p1 = csilk_json_object();
    csilk_json_add_string(p1, "sub", "user1");
    csilk_json_add_number(p1, "exp", (double)time(NULL) + 3600);
    char* t1 = csilk_jwt_generate(c, p1, secret);
    assert(t1 != NULL);

    csilk_jwt_options_t opts = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_REQUIRE_EXP,
        .leeway_sec = 0,
    };
    csilk_json_t* v1 = csilk_jwt_verify_options(c, t1, secret, strlen(secret), &opts);
    assert(v1 != NULL);
    csilk_json_free(v1);
    csilk_json_free(p1);
    free(t1);

    /* Token with past exp */
    csilk_json_t* p2 = csilk_json_object();
    csilk_json_add_string(p2, "sub", "user2");
    csilk_json_add_number(p2, "exp", (double)time(NULL) - 60);
    char* t2 = csilk_jwt_generate(c, p2, secret);
    assert(t2 != NULL);

    csilk_json_t* v2 = csilk_jwt_verify_options(c, t2, secret, strlen(secret), &opts);
    assert(v2 == NULL);

    /* Test leeway on slightly expired token */
    csilk_jwt_options_t opts_leeway = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_REQUIRE_EXP,
        .leeway_sec = 120, /* 120s clock skew tolerance */
    };
    csilk_json_t* v2_leeway = csilk_jwt_verify_options(c, t2, secret, strlen(secret), &opts_leeway);
    assert(v2_leeway != NULL);
    csilk_json_free(v2_leeway);

    csilk_json_free(p2);
    free(t2);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_nbf_and_iat_policy(void)
{
    printf("Testing JWT nbf and iat policy...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  secret = "secret";

    /* Token not yet valid (future nbf) */
    csilk_json_t* p1 = csilk_json_object();
    csilk_json_add_string(p1, "sub", "user1");
    csilk_json_add_number(p1, "nbf", (double)time(NULL) + 3600);
    char* t1 = csilk_jwt_generate(c, p1, secret);
    assert(t1 != NULL);

    csilk_jwt_options_t opts_none = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_NONE,
        .leeway_sec = 0,
    };
    /* Even with flags=0, present nbf is validated */
    csilk_json_t* v1 = csilk_jwt_verify_options(c, t1, secret, strlen(secret), &opts_none);
    assert(v1 == NULL);

    csilk_json_free(p1);
    free(t1);

    /* Missing nbf with REQUIRE_NBF flag */
    csilk_json_t* p2 = csilk_json_object();
    csilk_json_add_string(p2, "sub", "user2");
    char* t2 = csilk_jwt_generate(c, p2, secret);
    assert(t2 != NULL);

    csilk_jwt_options_t opts_req_nbf = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_REQUIRE_NBF,
        .leeway_sec = 0,
    };
    csilk_json_t* v2 = csilk_jwt_verify_options(c, t2, secret, strlen(secret), &opts_req_nbf);
    assert(v2 == NULL);

    csilk_json_free(p2);
    free(t2);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_generate_json(void)
{
    printf("Testing csilk_jwt_generate_json...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  secret = "my-secret-key-123";
    const char*  payload_json = "{\"sub\":\"12345\",\"role\":\"admin\"}";

    char* token = csilk_jwt_generate_json(c, payload_json, secret);
    assert(token != NULL);

    csilk_json_t* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != NULL);
    assert(strcmp(csilk_json_get_string(verified, "sub"), "12345") == 0);
    assert(strcmp(csilk_json_get_string(verified, "role"), "admin") == 0);

    csilk_json_free(verified);
    free(token);

    /* Invalid JSON returns NULL */
    char* invalid = csilk_jwt_generate_json(c, "not json", secret);
    assert(invalid == NULL);

    /* NULL payload or secret returns NULL */
    assert(csilk_jwt_generate_json(c, NULL, secret) == NULL);
    assert(csilk_jwt_generate_json(c, payload_json, NULL) == NULL);

    /* NULL context succeeds using default crypto driver */
    char* token_null_ctx = csilk_jwt_generate_json(NULL, payload_json, secret);
    assert(token_null_ctx != NULL);
    free(token_null_ctx);

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_rs256_roundtrip(void)
{
    printf("Testing JWT RS256 round-trip...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    char   pub_key[2048];
    char   priv_key[4096];
    size_t pub_len = sizeof(pub_key);
    size_t priv_len = sizeof(priv_key);

    int r = _csilk_generate_keypair(c, pub_key, &pub_len, priv_key, &priv_len);
    assert(r == 0);

    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "sub", "rs256_user");
    csilk_json_add_string(payload, "scope", "read write");
    csilk_json_add_number(payload, "exp", (double)time(NULL) + 3600);

    /* Generate with RSA private key */
    char* token = csilk_jwt_generate_ex(c, payload, priv_key, priv_len, CSILK_JWT_RS256);
    assert(token != NULL);

    /* Verify with RSA public key */
    csilk_json_t* verified = csilk_jwt_verify_ex(c, token, pub_key, pub_len, CSILK_JWT_RS256);
    assert(verified != NULL);
    assert(strcmp(csilk_json_get_string(verified, "sub"), "rs256_user") == 0);
    assert(strcmp(csilk_json_get_string(verified, "scope"), "read write") == 0);

    /* Verify with wrong key fails */
    csilk_json_t* wrong = csilk_jwt_verify_ex(c, token, "wrong-key", 9, CSILK_JWT_RS256);
    assert(wrong == NULL);

    /* Verify with wrong algorithm fails */
    csilk_json_t* wrong_alg = csilk_jwt_verify_ex(c, token, pub_key, pub_len, CSILK_JWT_HS256);
    assert(wrong_alg == NULL);

    csilk_json_free(wrong_alg);
    csilk_json_free(wrong);
    csilk_json_free(verified);
    csilk_json_free(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
dummy_next_handler(csilk_ctx_t* c)
{
    (void)c;
}

static void
test_jwt_middleware_valid(void)
{
    printf("Testing JWT middleware with valid Bearer token...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_next_handler, NULL};
    csilk_test_ctx_set_handlers(c, handlers);

    const char*   secret = "supersecret";
    csilk_json_t* payload = csilk_json_object();
    csilk_json_add_string(payload, "user", "admin");
    char* token = csilk_jwt_generate(c, payload, secret);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    csilk_set_request_header(c, "Authorization", auth_header);
    csilk_jwt_middleware(c, secret);
    assert(csilk_is_aborted(c) == 0);

    csilk_json_t* stored = (csilk_json_t*)csilk_get(c, "jwt_payload");
    assert(stored != NULL);
    assert(strcmp(csilk_json_get_string(stored, "user"), "admin") == 0);

    free(token);
    csilk_json_free(payload);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_middleware_require_exp(void)
{
    printf("Testing JWT middleware with CSILK_JWT_REQUIRE_EXP...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_next_handler, NULL};
    csilk_test_ctx_set_handlers(c, handlers);

    const char*   secret = "supersecret";
    csilk_json_t* payload_no_exp = csilk_json_object();
    csilk_json_add_string(payload_no_exp, "user", "admin");
    char* token_no_exp = csilk_jwt_generate(c, payload_no_exp, secret);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token_no_exp);

    csilk_set_request_header(c, "Authorization", auth_header);

    csilk_jwt_options_t opts = {
        .algorithm = CSILK_JWT_HS256,
        .flags = CSILK_JWT_REQUIRE_EXP,
        .leeway_sec = 0,
    };
    csilk_jwt_middleware_options(c, secret, strlen(secret), &opts);

    /* Must be aborted with 401 Unauthorized because exp is missing */
    assert(csilk_is_aborted(c) == 1);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);

    free(token_no_exp);
    csilk_json_free(payload_no_exp);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_middleware_missing(void)
{
    printf("Testing JWT middleware with missing Authorization header...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_next_handler, NULL};
    csilk_test_ctx_set_handlers(c, handlers);

    csilk_jwt_middleware(c, "secret");
    assert(csilk_is_aborted(c) == 1);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

int
main(void)
{
    test_jwt_hs256_roundtrip();
    test_jwt_wrong_secret();
    test_jwt_expired_token();
    test_jwt_malformed_token();
    test_jwt_no_exp_claim();
    test_jwt_require_exp_policy();
    test_jwt_nbf_and_iat_policy();
    test_jwt_generate_json();
    test_jwt_rs256_roundtrip();
    test_jwt_middleware_valid();
    test_jwt_middleware_require_exp();
    test_jwt_middleware_missing();

    printf("test_jwt_security: ALL PASSED\n");
    return 0;
}
