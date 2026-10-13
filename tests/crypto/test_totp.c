#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "csilk/core/totp.h"

void
test_rfc6238_test_vectors(void)
{
    /* RFC 6238 Appendix B / Table 1 vectors for SHA-1 */
    /* Secret: "12345678901234567890" -> Base32: "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ" */
    const char* secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    char        code[8];

    /* Time = 59 -> step = 1 -> "287082" */
    assert(csilk_totp_generate_code(secret, 59, code, sizeof(code)) == 0);
    assert(strcmp(code, "287082") == 0);

    /* Time = 1111111109 -> step = 37037036 -> "081804" */
    assert(csilk_totp_generate_code(secret, 1111111109, code, sizeof(code)) == 0);
    assert(strcmp(code, "081804") == 0);

    /* Time = 1111111111 -> step = 37037037 -> "050471" */
    assert(csilk_totp_generate_code(secret, 1111111111, code, sizeof(code)) == 0);
    assert(strcmp(code, "050471") == 0);

    /* Time = 1234567890 -> step = 41152263 -> "005924" */
    assert(csilk_totp_generate_code(secret, 1234567890, code, sizeof(code)) == 0);
    assert(strcmp(code, "005924") == 0);

    /* Time = 2000000000 -> step = 66666666 -> "279037" */
    assert(csilk_totp_generate_code(secret, 2000000000, code, sizeof(code)) == 0);
    assert(strcmp(code, "279037") == 0);

    printf("test_rfc6238_test_vectors passed\n");
}

void
test_totp_secret_and_verify(void)
{
    char secret[64];
    assert(csilk_totp_generate_secret(secret, sizeof(secret)) == 0);
    assert(strlen(secret) == 32);

    /* Compute current code */
    uint64_t now = (uint64_t)time(NULL);
    char     code[8];
    assert(csilk_totp_generate_code(secret, now, code, sizeof(code)) == 0);
    assert(strlen(code) == 6);

    /* Verification should succeed */
    assert(csilk_totp_verify_code(secret, code) == true);

    /* Invalid code should fail */
    assert(csilk_totp_verify_code(secret, "999999") == false || strcmp(code, "999999") == 0);
    assert(csilk_totp_verify_code(secret, "123") == false);
    assert(csilk_totp_verify_code(NULL, code) == false);

    printf("test_totp_secret_and_verify passed\n");
}

void
test_totp_build_uri(void)
{
    char uri[256];
    assert(csilk_totp_build_uri(
               "MyApp", "alice@example.com", "JBSWY3DPEHPK3PXP", uri, sizeof(uri)) == 0);
    assert(strstr(uri, "otpauth://totp/MyApp:alice@example.com") != NULL);
    assert(strstr(uri, "secret=JBSWY3DPEHPK3PXP") != NULL);
    assert(strstr(uri, "issuer=MyApp") != NULL);

    printf("test_totp_build_uri passed\n");
}

void
test_totp_backup_codes(void)
{
    char codes[8][16];
    assert(csilk_totp_generate_backup_codes(codes) == 0);
    for (int i = 0; i < 8; i++) {
        assert(strlen(codes[i]) == 9);
        assert(codes[i][4] == '-');
    }

    /* JSON serialization mock */
    char json[512];
    snprintf(json, sizeof(json), "[\"%s\",\"%s\"]", codes[0], codes[1]);

    char updated[512];
    /* Consume first code with hyphen */
    assert(csilk_totp_verify_and_consume_backup_code(json, codes[0], updated, sizeof(updated)) ==
           true);
    assert(strstr(updated, codes[0]) == NULL);
    assert(strstr(updated, codes[1]) != NULL);

    /* Try to consume same code again should fail */
    char updated2[512];
    assert(csilk_totp_verify_and_consume_backup_code(
               updated, codes[0], updated2, sizeof(updated2)) == false);

    printf("test_totp_backup_codes passed\n");
}

int
main(void)
{
    test_rfc6238_test_vectors();
    test_totp_secret_and_verify();
    test_totp_build_uri();
    test_totp_backup_codes();
    printf("test_totp: ALL PASSED\n");
    return 0;
}
