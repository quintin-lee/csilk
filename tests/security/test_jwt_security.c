#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "csilk/test/test.h"

static void
test_jwt_hs256_roundtrip(void)
{
    printf("Testing JWT HS256 round-trip...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "user123");
    cJSON_AddStringToObject(payload, "role", "admin");
    cJSON_AddNumberToObject(payload, "iat", 1700000000);

    const char* secret = "my-secret-key-for-testing";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != nullptr);

    cJSON* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != nullptr);
    assert(strcmp(cJSON_GetObjectItem(verified, "sub")->valuestring, "user123") == 0);
    assert(strcmp(cJSON_GetObjectItem(verified, "role")->valuestring, "admin") == 0);

    cJSON_Delete(verified);
    cJSON_Delete(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_wrong_secret(void)
{
    printf("Testing JWT wrong secret rejection...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "test");

    char* token = csilk_jwt_generate(c, payload, "correct-secret");
    assert(token != nullptr);

    cJSON* verified = csilk_jwt_verify(c, token, "wrong-secret");
    assert(verified == nullptr);

    cJSON_Delete(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_expired_token(void)
{
    printf("Testing JWT expired token (verify checks signature, not exp)...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "user");
    cJSON_AddNumberToObject(payload, "exp", 1000000);

    const char* secret = "test-secret";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != nullptr);

    cJSON* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != nullptr);
    assert(cJSON_GetObjectItem(verified, "exp") != nullptr);
    assert(cJSON_GetObjectItem(verified, "exp")->valueint == 1000000);

    cJSON_Delete(verified);
    cJSON_Delete(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_malformed_token(void)
{
    printf("Testing JWT malformed token rejection...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    cJSON* v1 = csilk_jwt_verify(c, "not-a-jwt", "secret");
    assert(v1 == nullptr);

    cJSON* v2 = csilk_jwt_verify(c, "only.two", "secret");
    assert(v2 == nullptr);

    cJSON* v3 = csilk_jwt_verify(c, "", "secret");
    assert(v3 == nullptr);

    cJSON* v4 = csilk_jwt_verify(c, "a.b.c.d", "secret");
    assert(v4 == nullptr);

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_no_exp_claim(void)
{
    printf("Testing JWT without exp claim...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    cJSON*       payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "user");
    cJSON_AddStringToObject(payload, "name", "Test User");

    const char* secret = "secret";
    char*       token = csilk_jwt_generate(c, payload, secret);
    assert(token != nullptr);

    cJSON* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != nullptr);
    assert(strcmp(cJSON_GetObjectItem(verified, "sub")->valuestring, "user") == 0);

    cJSON_Delete(verified);
    cJSON_Delete(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_generate_json(void)
{
    printf("Testing JWT generate from JSON string...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();
    const char*  secret = "json-secret";

    char* token = csilk_jwt_generate_json(c, "{\"sub\":\"json-user\",\"iat\":1700000000}", secret);
    assert(token != nullptr);

    cJSON* verified = csilk_jwt_verify(c, token, secret);
    assert(verified != nullptr);
    assert(strcmp(cJSON_GetObjectItem(verified, "sub")->valuestring, "json-user") == 0);

    cJSON_Delete(verified);
    free(token);
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

    const uint8_t data[] = "header.payload-to-sign";
    size_t        data_len = strlen((const char*)data);

    uint8_t sig[256];
    size_t  sig_len = sizeof(sig);

    r = _csilk_jwt_sign(c, priv_key, priv_len, data, data_len, sig, &sig_len, CSILK_JWT_RS256);
    assert(r == 0);
    assert(sig_len > 0);

    r = _csilk_jwt_verify(c, pub_key, pub_len, data, data_len, sig, sig_len, CSILK_JWT_RS256);
    assert(r == 0);

    sig[0] ^= 0xFF;
    r = _csilk_jwt_verify(c, pub_key, pub_len, data, data_len, sig, sig_len, CSILK_JWT_RS256);
    assert(r == -1);

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
}

static void
test_jwt_middleware_valid(void)
{
    printf("Testing JWT middleware with valid token...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_handler, nullptr};
    csilk_test_ctx_set_handlers(c, handlers);

    const char* secret = "middleware-secret";

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "user1");
    cJSON_AddNumberToObject(payload, "iat", (double)time(nullptr));

    char* token = csilk_jwt_generate(c, payload, secret);
    assert(token != nullptr);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    csilk_set_request_header(c, "Authorization", auth_header);

    csilk_jwt_middleware(c, secret);
    assert(csilk_is_aborted(c) == 0);

    cJSON* stored = (cJSON*)csilk_get(c, "jwt_payload");
    assert(stored != nullptr);
    cJSON_Delete(stored);

    cJSON_Delete(payload);
    free(token);
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_jwt_middleware_missing(void)
{
    printf("Testing JWT middleware with missing token...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_handler, nullptr};
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
    test_jwt_generate_json();
    test_jwt_rs256_roundtrip();
    test_jwt_middleware_valid();
    test_jwt_middleware_missing();

    printf("test_jwt_security: ALL PASSED\n");
    return 0;
}
