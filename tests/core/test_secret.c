#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/core/secret.h"

static int
mock_secret_manager(const char* key, char* out, size_t out_size)
{
    if (strcmp(key, "VAULT_KEY") == 0) {
        snprintf(out, out_size, "vault-token-xyz");
        return 0;
    }
    return -1;
}

static void
test_test_overrides(void)
{
    csilk_secret_clear_test_overrides();
    csilk_secret_set_test_override("JWT_SECRET", "super-secret-jwt-key");

    char        buf[128];
    const char* val = csilk_secret_get("JWT_SECRET", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "super-secret-jwt-key") == 0);

    /* Test prefix matching */
    csilk_secret_set_prefix("MINEFOLIO");
    val = csilk_secret_get("MINEFOLIO_JWT_SECRET", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "super-secret-jwt-key") == 0);

    /* Clear overrides */
    csilk_secret_clear_test_overrides();
    csilk_secret_set_prefix(NULL);
}

static void
test_env_and_file_provider(void)
{
    /* Environment lookup */
    setenv("CSILK_DB_PASS", "db-pass-secure", 1);
    char        buf[128];
    const char* val = csilk_secret_get("DB_PASS", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "db-pass-secure") == 0);
    unsetenv("CSILK_DB_PASS");

    /* File lookup */
    char tmp_file[] = "/tmp/csilk_sec_XXXXXX";
    int  fd = mkstemp(tmp_file);
    assert(fd >= 0);
    const char* file_content = "secret-from-disk\r\n";
    assert(write(fd, file_content, strlen(file_content)) == (ssize_t)strlen(file_content));
    close(fd);

    setenv("CSILK_DISK_KEY_FILE", tmp_file, 1);
    val = csilk_secret_get("DISK_KEY", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "secret-from-disk") == 0);

    unsetenv("CSILK_DISK_KEY_FILE");
    unlink(tmp_file);
}

static void
test_external_manager(void)
{
    csilk_secret_set_manager(mock_secret_manager);

    char        buf[128];
    const char* val = csilk_secret_get("VAULT_KEY", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "vault-token-xyz") == 0);

    val = csilk_secret_get("NON_EXISTENT_KEY", buf, sizeof(buf));
    assert(val == NULL);

    csilk_secret_set_manager(NULL);
}

static void
test_safety_circuit_breaker(void)
{
    setenv("CSILK_DANGEROUS_SECRET", "please-change-me-later", 1);

    /* In production mode, forbidden pattern must trip circuit breaker */
    csilk_secret_set_test_mode(false);
    assert(csilk_secret_is_test_mode() == false);

    char        buf[128];
    const char* val = csilk_secret_get("DANGEROUS_SECRET", buf, sizeof(buf));
    assert(val == NULL);
    assert(csilk_secret_is_valid("DANGEROUS_SECRET") == false);

    /* In test mode, allowed */
    csilk_secret_set_test_mode(true);
    assert(csilk_secret_is_test_mode() == true);
    val = csilk_secret_get("DANGEROUS_SECRET", buf, sizeof(buf));
    assert(val != NULL);
    assert(strcmp(val, "please-change-me-later") == 0);

    unsetenv("CSILK_DANGEROUS_SECRET");
}

static void
test_env_get_fallback(void)
{
    char        buf[128];
    const char* val = csilk_env_get("NON_EXISTENT_CFG", buf, sizeof(buf), "default-val-8080");
    assert(val != NULL);
    assert(strcmp(val, "default-val-8080") == 0);
}

int
main(void)
{
    test_test_overrides();
    test_env_and_file_provider();
    test_external_manager();
    test_safety_circuit_breaker();
    test_env_get_fallback();
    return 0;
}
