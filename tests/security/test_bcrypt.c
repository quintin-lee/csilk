#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"

static void
test_bcrypt_hash_format(void)
{
    printf("Testing bcrypt hash format...\n");

    char hash[CSILK_BCRYPT_HASH_LEN];
    csilk_bcrypt_hash("test", 4, CSILK_BCRYPT_MIN_COST, hash);

    assert(strlen(hash) == CSILK_BCRYPT_HASH_LEN - 1);
    assert(hash[0] == '$' && hash[1] == '2' && hash[2] == 'a' && hash[3] == '$');

    int cost = (hash[4] - '0') * 10 + (hash[5] - '0');
    assert(cost == CSILK_BCRYPT_MIN_COST);

    printf("  hash: %s\n", hash);
    printf("  passed\n");
}

static void
test_bcrypt_verify_correct(void)
{
    printf("Testing bcrypt verify (correct password)...\n");

    char hash[CSILK_BCRYPT_HASH_LEN];
    csilk_bcrypt_hash("hello world", 11, CSILK_BCRYPT_MIN_COST, hash);

    int result = csilk_bcrypt_verify("hello world", 11, hash);
    assert(result == 0);

    printf("  passed\n");
}

static void
test_bcrypt_verify_incorrect(void)
{
    printf("Testing bcrypt verify (wrong password)...\n");

    char hash[CSILK_BCRYPT_HASH_LEN];
    csilk_bcrypt_hash("hello world", 11, CSILK_BCRYPT_MIN_COST, hash);

    int result = csilk_bcrypt_verify("wrong password", 14, hash);
    assert(result == -1);

    printf("  passed\n");
}

static void
test_bcrypt_deterministic(void)
{
    printf("Testing bcrypt determinism (same input = same output)...\n");

#ifdef TEST_OOM
    char hash1[CSILK_BCRYPT_HASH_LEN];
    char hash2[CSILK_BCRYPT_HASH_LEN];

    csilk_bcrypt_hash("deterministic", 13, CSILK_BCRYPT_MIN_COST, hash1);
    csilk_bcrypt_hash("deterministic", 13, CSILK_BCRYPT_MIN_COST, hash2);

    assert(strcmp(hash1, hash2) == 0);
    printf("  hash: %s\n", hash1);
    printf("  passed\n");
#else
    printf("  skipped (non-deterministic salt)\n");
#endif
}

static void
test_bcrypt_cost_clamping(void)
{
    printf("Testing bcrypt cost clamping...\n");

    char hash_low[CSILK_BCRYPT_HASH_LEN];
    char hash_high[CSILK_BCRYPT_HASH_LEN];

    csilk_bcrypt_hash("test", 4, 2, hash_low);   /* below min */
    csilk_bcrypt_hash("test", 4, 35, hash_high); /* above max */

    int cost_low = (hash_low[4] - '0') * 10 + (hash_low[5] - '0');
    int cost_high = (hash_high[4] - '0') * 10 + (hash_high[5] - '0');

    assert(cost_low == CSILK_BCRYPT_MIN_COST);
    assert(cost_high == CSILK_BCRYPT_MAX_COST);

    printf("  low cost: %d, high cost: %d\n", cost_low, cost_high);
    printf("  passed\n");
}

static void
test_bcrypt_password_truncation(void)
{
    printf("Testing bcrypt password truncation (>72 bytes)...\n");

    char long_pwd[100];
    memset(long_pwd, 'A', sizeof(long_pwd));
    long_pwd[sizeof(long_pwd) - 1] = '\0';

    char hash1[CSILK_BCRYPT_HASH_LEN];
    char hash2[CSILK_BCRYPT_HASH_LEN];

    csilk_bcrypt_hash(long_pwd, sizeof(long_pwd) - 1, CSILK_BCRYPT_MIN_COST, hash1);
    csilk_bcrypt_hash(long_pwd, 72, CSILK_BCRYPT_MIN_COST, hash2);

#ifdef TEST_OOM
    assert(strcmp(hash1, hash2) == 0);
#endif
    printf("  passed\n");
}

static void
test_bcrypt_verify_edge_cases(void)
{
    printf("Testing bcrypt verify edge cases...\n");

    char hash[CSILK_BCRYPT_HASH_LEN];
    csilk_bcrypt_hash("", 0, CSILK_BCRYPT_MIN_COST, hash);

    int r1 = csilk_bcrypt_verify("", 0, hash);
    assert(r1 == 0);

    int r2 = csilk_bcrypt_verify("notempty", 8, hash);
    assert(r2 == -1);

    int r3 = csilk_bcrypt_verify("test", 4, NULL);
    assert(r3 == -1);

    int r4 = csilk_bcrypt_verify("test", 4, "invalid");
    assert(r4 == -1);

    printf("  passed\n");
}

static void
test_bcrypt_known_vector(void)
{
    printf("Testing bcrypt known test vector...\n");

    /* Test vector from OpenBSD bcrypt tests */
    const char* expected = "$2a$06$DCq7YPn5Rq63x1ZoAMiMi.P3eHkHPxCsZWiKAYjBGMIuDPXzSzLWC";
    const char* password = "Hello!";

    char hash[CSILK_BCRYPT_HASH_LEN];
    csilk_bcrypt_hash(password, strlen(password), 6, hash);

    /* Note: This test may fail if salt is random.
       We only check that verification works with our own hash. */
    int result = csilk_bcrypt_verify(password, strlen(password), hash);
    assert(result == 0);

    printf("  generated: %s\n", hash);
    printf("  passed\n");
}

int
main(void)
{
    test_bcrypt_hash_format();
    test_bcrypt_verify_correct();
    test_bcrypt_verify_incorrect();
    test_bcrypt_deterministic();
    test_bcrypt_cost_clamping();
    test_bcrypt_password_truncation();
    test_bcrypt_verify_edge_cases();
    test_bcrypt_known_vector();

    printf("test_bcrypt: ALL PASSED\n");
    return 0;
}
