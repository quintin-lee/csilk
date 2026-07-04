/**
 * @file test_jwt.c
 * @brief Unit tests for JWT generation and verification.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "csilk/core/internal.h"

void
test_jwt_core()
{
    printf("Testing JWT core generation and verification...\n");

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "1234567890");
    cJSON_AddStringToObject(payload, "name", "John Doe");
    cJSON_AddNumberToObject(payload, "iat", 1516239022);

    const char* secret = "secret";

    csilk_ctx_t* c = csilk_test_ctx_new();

    char* token = csilk_jwt_generate(c, payload, secret);
    assert(token != nullptr);
    printf("Generated Token: %s\n", token);

    cJSON* verified_payload = csilk_jwt_verify(c, token, secret);
    assert(verified_payload != nullptr);

    assert(cJSON_HasObjectItem(verified_payload, "sub"));
    assert(strcmp(cJSON_GetObjectItem(verified_payload, "sub")->valuestring, "1234567890") == 0);

    // Test invalid secret
    cJSON* invalid_payload = csilk_jwt_verify(c, token, "wrong_secret");
    assert(invalid_payload == nullptr);

    // Test tampered token
    token[strlen(token) - 1] ^= 0xFF; // Flip a bit in signature
    cJSON* tampered_payload = csilk_jwt_verify(c, token, secret);
    assert(tampered_payload == nullptr);

    free(token);
    cJSON_Delete(payload);
    cJSON_Delete(verified_payload);
    csilk_test_ctx_free(c);
}

void
dummy_handler(csilk_ctx_t* c)
{
    (void)c;
}

void
test_jwt_middleware()
{
    printf("Testing JWT middleware...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    // Setup handlers for csilk_next
    csilk_handler_t handlers[] = {dummy_handler, dummy_handler, nullptr};
    csilk_test_ctx_set_handlers(c, handlers);

    const char* secret = "supersecret";
    cJSON*      payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "user", "admin");
    char* token = csilk_jwt_generate(c, payload, secret);

    char auth_header[512];
    sprintf(auth_header, "Bearer %s", token);

    // 1. Success case
    csilk_set_request_header(c, "Authorization", auth_header);
    csilk_jwt_middleware(c, secret);
    assert(csilk_is_aborted(c) == 0);

    cJSON* stored_payload = (cJSON*)csilk_get(c, "jwt_payload");
    assert(stored_payload != nullptr);
    assert(strcmp(cJSON_GetObjectItem(stored_payload, "user")->valuestring, "admin") == 0);
    cJSON_Delete(stored_payload);

    // 2. Failure case: missing header
    csilk_test_ctx_free(c);
    c = csilk_test_ctx_new();
    csilk_test_ctx_set_handlers(c, handlers);
    csilk_jwt_middleware(c, secret);
    assert(csilk_is_aborted(c) == 1);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);

    // 3. Failure case: invalid token
    csilk_test_ctx_free(c);
    c = csilk_test_ctx_new();
    csilk_test_ctx_set_handlers(c, handlers);
    csilk_set_request_header(c, "Authorization", "Bearer invalid.token.here");
    csilk_jwt_middleware(c, secret);
    assert(csilk_is_aborted(c) == 1);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);

    free(token);
    cJSON_Delete(payload);
    csilk_test_ctx_free(c);
}

void
test_jwt_expiration()
{
    printf("Testing JWT expiration...\n");

    csilk_ctx_t*    c = csilk_test_ctx_new();
    csilk_handler_t handlers[] = {dummy_handler, nullptr};
    csilk_test_ctx_set_handlers(c, handlers);

    const char* secret = "secret";
    cJSON*      payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "exp",
                            (double)time(nullptr) - 10); // Expired 10s ago

    char* token = csilk_jwt_generate(c, payload, secret);
    char  auth_header[512];
    sprintf(auth_header, "Bearer %s", token);

    csilk_set_request_header(c, "Authorization", auth_header);
    csilk_jwt_middleware(c, secret);

    assert(csilk_is_aborted(c) == 1);
    assert(csilk_get_status(c) == CSILK_STATUS_UNAUTHORIZED);

    free(token);
    cJSON_Delete(payload);
    csilk_test_ctx_free(c);
}

static const char* rsa_private_key =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDLYVVQlRfPiyi6\n"
    "PY8ceujxTY1JggzxGeB9atYyeqdSjtooRlbEdtBVDw83Z25c55EyZnuVkB/tm7VE\n"
    "tE+wyI9cL+EQqeVbpdMnvmqMb86ExSXChEtj71Ag423NPD5ngFbYqOUKmXpOzea1\n"
    "/4l633MyEk0PjTLx0w3U3cJ2LUfTTLF1pRANkW8r8zJldyrKgaifz+WUSJSb/HsW\n"
    "C4pIK+Aj4GsdF8YQuqYZYY2xqEPBCnZgHauWxxbCMz6xtdxixVmGvrhLPzlDPkjp\n"
    "Iggkrl/NS+7sX2I5RFZA0LTObIlObzRR4Q6rRmpCIc5POvegtFAc4oQNZcSNPQve\n"
    "lBGdTXk9AgMBAAECggEAIgF+UGD9gDhWcP2GBk8Oz/oVm6rhGxcmkBHjCsGzZHx6\n"
    "Qa2xKFxtbSt9cdgbffFCN9km5NUaYZJddsVnZbnDLrjauvyNWwuZCNYv3pd7Qnvs\n"
    "Wl/gcy86iKU+YMin7opo+wUSdvk+mBqZbujxRdhC3KZuuCD12bVeZK9HqpbboZqu\n"
    "rFS9RM+CE/8k4DYqTmLxCrpZDyHjvjrdpYIZU2lIwRHDV9svJx3A7yK2aiojsJqm\n"
    "7OTlMv+Ea/bzdd4/THGOCxI4U6Jx52BAZcExk/M1LgDhtc9gs+69dJJeUmQURqeg\n"
    "jfvsjGO8LZ+RTKEZqe5RS9k5DKQCDwDYoKILQEGByQKBgQD3PJ4yXLwkFsMhW6vN\n"
    "Sd35fcKHSUNv3fWo+JYGbpaJh9VJ1fgkxaB0K3OyuMuYHPm1si/FY83KDGFTW4ya\n"
    "AIY5W7rEvjzar6Urkbs5RcXtc+Xr8RHlHb6BT/KzqiTk624eHGPDaOraktALMGqV\n"
    "wYmBBgIeSeXtGtqnBMoG4s7IgwKBgQDSlsTC1cYKYb1Wz+EUadsQszzMSwtuSGiR\n"
    "6GL+LzbD72MCB0lUtLlJ4MLhQmR0EZ/45hh2ZDQ7Y0YKn1Zp/U2ZQvqG981NSvxQ\n"
    "jPTpMj64IFjjkSratiUgskLoOFybBlpk/pPlWN83QGcj4YDCDVSxCsNQdfW1pE+e\n"
    "nRRJ8FmLPwKBgCr61cGJj4dykY9+ATrZ6YXSz/t2yAtteaRbOrF5jh/whiqk0NOL\n"
    "q54mY6GhMHuMJfjpNhbJh9/lERJNqv6msq7L/IbxT2DxAfS2C+cj8wmZiVHgAa1j\n"
    "41dVj6qeHHXTW7xOUSWKWrGOri4Tx6OrFn1gjwO28wqqDXLViU1zJmGDAoGAUNig\n"
    "4UvAo+uyDMnx1yxsdZTaGnQVB1m1C47zsjHeDIqyr+ysMmDPYZVwO5qJhiXeDGgJ\n"
    "rCn8A3CxSxKw0i/0won8NCSeJLZM93+l5oDrozSH65Wnph+XUV4eYZiBtOJTgcJa\n"
    "dQoRZ9zJu/SuwdDsWquPICypD/rstjAHwfsL5XECgYBkXV2HktGAUywrnadSUZI6\n"
    "R+CsC4wQDL1dURcKcdr6HpmyYaG7rQWEFTCfyF572lq4RvXjm/vKygd1u0ig4C9M\n"
    "HdQ85wEMRHZy8kYZh62O3LFOpqNIcCEFS54UgKaD7rlVQlRwqUWuJwMhyDUNIta/\n"
    "9el5T28W/BEo0Pz4kYzb4g==\n"
    "-----END PRIVATE KEY-----\n";

static const char* rsa_public_key =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAy2FVUJUXz4souj2PHHro\n"
    "8U2NSYIM8RngfWrWMnqnUo7aKEZWxHbQVQ8PN2duXOeRMmZ7lZAf7Zu1RLRPsMiP\n"
    "XC/hEKnlW6XTJ75qjG/OhMUlwoRLY+9QIONtzTw+Z4BW2KjlCpl6Ts3mtf+Jet9z\n"
    "MhJND40y8dMN1N3Cdi1H00yxdaUQDZFvK/MyZXcqyoGon8/llEiUm/x7FguKSCvg\n"
    "I+BrHRfGELqmGWGNsahDwQp2YB2rlscWwjM+sbXcYsVZhr64Sz85Qz5I6SIIJK5f\n"
    "zUvu7F9iOURWQNC0zmyJTm80UeEOq0ZqQiHOTzr3oLRQHOKEDWXEjT0L3pQRnU15\n"
    "PQIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

static const char* ec_private_key =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgFJbY+0ZvXGNzRGYU\n"
    "WMZ9Js/TnyDw3WV05eaumUQAjhKhRANCAAR9SUnitNedzWPG/HaABeQh7S0qmACG\n"
    "DDciLEPCiraVOzE+OBEl42ZrCBvBscDpzSw+uzBmpl1n/6W6TT+18CvV\n"
    "-----END PRIVATE KEY-----\n";

static const char* ec_public_key =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEfUlJ4rTXnc1jxvx2gAXkIe0tKpgA\n"
    "hgw3IixDwoq2lTsxPjgRJeNmawgbwbHA6c0sPrswZqZdZ/+luk0/tfAr1Q==\n"
    "-----END PUBLIC KEY-----\n";

void
test_jwt_rs256()
{
    printf("Testing JWT RS256 generation and verification...\n");

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "admin-rs256");

    csilk_ctx_t* c = csilk_test_ctx_new();

    char* token = csilk_jwt_generate_ex(
        c, payload, rsa_private_key, strlen(rsa_private_key), CSILK_JWT_RS256);
    assert(token != nullptr);

    cJSON* verified_payload =
        csilk_jwt_verify_ex(c, token, rsa_public_key, strlen(rsa_public_key), CSILK_JWT_RS256);
    assert(verified_payload != nullptr);
    assert(cJSON_HasObjectItem(verified_payload, "sub"));
    assert(strcmp(cJSON_GetObjectItem(verified_payload, "sub")->valuestring, "admin-rs256") == 0);

    // Test invalid public key
    cJSON* invalid_payload =
        csilk_jwt_verify_ex(c, token, ec_public_key, strlen(ec_public_key), CSILK_JWT_RS256);
    assert(invalid_payload == nullptr);

    free(token);
    cJSON_Delete(payload);
    cJSON_Delete(verified_payload);
    csilk_test_ctx_free(c);
}

void
test_jwt_es256()
{
    printf("Testing JWT ES256 generation and verification...\n");

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", "admin-es256");

    csilk_ctx_t* c = csilk_test_ctx_new();

    char* token =
        csilk_jwt_generate_ex(c, payload, ec_private_key, strlen(ec_private_key), CSILK_JWT_ES256);
    assert(token != nullptr);

    cJSON* verified_payload =
        csilk_jwt_verify_ex(c, token, ec_public_key, strlen(ec_public_key), CSILK_JWT_ES256);
    assert(verified_payload != nullptr);
    assert(cJSON_HasObjectItem(verified_payload, "sub"));
    assert(strcmp(cJSON_GetObjectItem(verified_payload, "sub")->valuestring, "admin-es256") == 0);

    // Test invalid public key
    cJSON* invalid_payload =
        csilk_jwt_verify_ex(c, token, rsa_public_key, strlen(rsa_public_key), CSILK_JWT_ES256);
    assert(invalid_payload == nullptr);

    free(token);
    cJSON_Delete(payload);
    cJSON_Delete(verified_payload);
    csilk_test_ctx_free(c);
}

int
main()
{
    test_jwt_core();
    test_jwt_middleware();
    test_jwt_expiration();
    test_jwt_rs256();
    test_jwt_es256();
    printf("All JWT tests passed!\n");
    return 0;
}
