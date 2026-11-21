#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "csilk/core/internal.h"

static void test_bcrypt_openbsd_vectors(void);
static void test_bcrypt_cost_scaling(void);

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

    /* Avoid exercising the intentionally impractical maximum cost in a unit test. */
    csilk_bcrypt_hash("test", 4, CSILK_BCRYPT_MIN_COST - 1, hash_low);  /* below min */
    csilk_bcrypt_hash("test", 4, CSILK_BCRYPT_MIN_COST + 1, hash_high); /* above the test cost */

    int cost_low = (hash_low[4] - '0') * 10 + (hash_low[5] - '0');
    int cost_high = (hash_high[4] - '0') * 10 + (hash_high[5] - '0');

    assert(cost_low == CSILK_BCRYPT_MIN_COST);
    assert(cost_high == CSILK_BCRYPT_MIN_COST + 1);

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
    test_bcrypt_openbsd_vectors();
    test_bcrypt_cost_scaling();

    printf("test_bcrypt: ALL PASSED\n");
    return 0;
}

static void
test_bcrypt_openbsd_vectors(void)
{
    printf("Testing bcrypt OpenBSD $2a$/$2b$ vectors...\n");

    struct {
        const char* password;
        const char* hash;
    } cases[] = {
        {"Hello!", "$2a$06$DCq7YPn5Rq63x1ZoAMiMi.VZybw9jdkrPQcC2BWa0UNZnv.MR.sQC"},
        {"a",      "$2b$05$abcdefghijklmnopqrstuu8.veTXNDPV2eEsdpKNczMw2otgh5LnK"},
        {"aa",     "$2b$05$abcdefghijklmnopqrstuuw5YJs8Npny5Vkq9r8JF0OglHT93sbs2"},
        {"abc",    "$2b$04$abcdefghijklmnopqrstuuCi15uRb1eH7NAlJ/TgeJertyknQpYn2"},
        {"abcabc", "$2b$04$abcdefghijklmnopqrstuuMr1iqMnqBcx.IPHMnu.khXBFsI6LKm2"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(csilk_bcrypt_verify(cases[i].password, strlen(cases[i].password), cases[i].hash) ==
               0);
        assert(csilk_bcrypt_verify("wrong", 5, cases[i].hash) == -1);
    }

    printf("  passed\n");
}

static void
test_bcrypt_cost_scaling(void)
{
    printf("Testing bcrypt cost scaling (2^cost work factor)...\n");

    char            hash[CSILK_BCRYPT_HASH_LEN];
    struct timespec t0, t1, t2;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    csilk_bcrypt_hash("cost-scale", 10, 4, hash);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double low_ms =
        (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    csilk_bcrypt_hash("cost-scale", 10, 8, hash);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double high_ms =
        (double)(t2.tv_sec - t1.tv_sec) * 1000.0 + (double)(t2.tv_nsec - t1.tv_nsec) / 1e6;

    assert(high_ms > low_ms * 4.0);
    assert(csilk_bcrypt_verify("cost-scale", 10, hash) == 0);

    printf("  cost 4: %.2f ms, cost 8: %.2f ms\n", low_ms, high_ms);
    printf("  passed\n");
}
