#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "csilk/core/secret.h"
#include "csilk/core/sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEST_OVERRIDES 32
#define TLS_BUF_SIZE 2048

typedef struct {
    char key[128];
    char value[1024];
} test_override_t;

static csilk_mutex_t           s_secret_lock;
static csilk_once_t            s_secret_once = CSILK_ONCE_INIT;
static csilk_secret_manager_fn s_secret_manager = NULL;
static char                    s_custom_prefix[64] = {0};
static test_override_t         s_test_overrides[MAX_TEST_OVERRIDES];
static size_t                  s_override_count = 0;
static bool                    s_test_mode = false;
static bool                    s_test_mode_initialized = false;

static _Thread_local char   s_tls_secret_ring[4][TLS_BUF_SIZE];
static _Thread_local size_t s_tls_secret_idx = 0;
static _Thread_local char   s_tls_env_ring[4][TLS_BUF_SIZE];
static _Thread_local size_t s_tls_env_idx = 0;

static const char* const FORBIDDEN_PATTERNS[] = {
    "change-me", "default-secret", "hard-coded-secret", "replace-with-", NULL};

static void
secret_lock_init(void)
{
    csilk_mutex_init(&s_secret_lock);
}

static void
ensure_lock_init(void)
{
    csilk_once(&s_secret_once, secret_lock_init);
}

static void
init_test_mode_from_env(void)
{
    if (s_test_mode_initialized) {
        return;
    }
    const char* env_test = getenv("CSILK_TEST");
    const char* env_ci = getenv("CI");
    if ((env_test && env_test[0] && strcmp(env_test, "0") != 0) ||
        (env_ci && env_ci[0] && strcmp(env_ci, "0") != 0)) {
        s_test_mode = true;
    }
    s_test_mode_initialized = true;
}

void
csilk_secret_set_test_mode(bool enabled)
{
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    s_test_mode = enabled;
    s_test_mode_initialized = true;
    csilk_mutex_unlock(&s_secret_lock);
}

bool
csilk_secret_is_test_mode(void)
{
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    init_test_mode_from_env();
    bool mode = s_test_mode;
    csilk_mutex_unlock(&s_secret_lock);
    return mode;
}

void
csilk_secret_set_manager(csilk_secret_manager_fn fn)
{
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    s_secret_manager = fn;
    csilk_mutex_unlock(&s_secret_lock);
}

void
csilk_secret_set_prefix(const char* prefix)
{
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    if (prefix && prefix[0]) {
        strncpy(s_custom_prefix, prefix, sizeof(s_custom_prefix) - 1);
        s_custom_prefix[sizeof(s_custom_prefix) - 1] = '\0';
        /* Strip trailing underscore if present */
        size_t l = strlen(s_custom_prefix);
        if (l > 0 && s_custom_prefix[l - 1] == '_') {
            s_custom_prefix[l - 1] = '\0';
        }
    } else {
        s_custom_prefix[0] = '\0';
    }
    csilk_mutex_unlock(&s_secret_lock);
}

void
csilk_secret_set_test_override(const char* key, const char* value)
{
    if (!key || !key[0]) {
        return;
    }
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    s_test_mode = true;
    s_test_mode_initialized = true;

    for (size_t i = 0; i < s_override_count; i++) {
        if (strcasecmp(s_test_overrides[i].key, key) == 0) {
            if (value) {
                strncpy(s_test_overrides[i].value, value, sizeof(s_test_overrides[i].value) - 1);
                s_test_overrides[i].value[sizeof(s_test_overrides[i].value) - 1] = '\0';
            } else {
                s_test_overrides[i] = s_test_overrides[s_override_count - 1];
                s_override_count--;
            }
            csilk_mutex_unlock(&s_secret_lock);
            return;
        }
    }

    if (value && s_override_count < MAX_TEST_OVERRIDES) {
        strncpy(s_test_overrides[s_override_count].key,
                key,
                sizeof(s_test_overrides[s_override_count].key) - 1);
        s_test_overrides[s_override_count].key[sizeof(s_test_overrides[s_override_count].key) - 1] =
            '\0';
        strncpy(s_test_overrides[s_override_count].value,
                value,
                sizeof(s_test_overrides[s_override_count].value) - 1);
        s_test_overrides[s_override_count]
            .value[sizeof(s_test_overrides[s_override_count].value) - 1] = '\0';
        s_override_count++;
    }
    csilk_mutex_unlock(&s_secret_lock);
}

void
csilk_secret_clear_test_overrides(void)
{
    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    s_override_count = 0;
    csilk_mutex_unlock(&s_secret_lock);
}

static bool
contains_forbidden_pattern(const char* val)
{
    if (!val) {
        return false;
    }
    for (int i = 0; FORBIDDEN_PATTERNS[i] != NULL; i++) {
        if (strcasestr(val, FORBIDDEN_PATTERNS[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static int
read_secret_from_file(const char* filepath, char* out, size_t out_size)
{
    if (!filepath || !filepath[0] || !out || out_size == 0) {
        return -1;
    }
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }

    size_t n = fread(out, 1, out_size - 1, fp);
    fclose(fp);
    out[n] = '\0';

    while (n > 0 &&
           (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }

    return (n > 0) ? 0 : -1;
}

const char*
csilk_secret_get(const char* key, char* out, size_t out_size)
{
    if (!key || !key[0]) {
        return NULL;
    }

    char*  target_buf = out ? out : s_tls_secret_ring[(s_tls_secret_idx++) & 3];
    size_t target_sz = out ? out_size : sizeof(s_tls_secret_ring[0]);
    target_buf[0] = '\0';

    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    init_test_mode_from_env();
    bool is_test = s_test_mode;
    char prefix[64];
    strncpy(prefix, s_custom_prefix, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';

    /* 1. In-memory test overrides */
    for (size_t i = 0; i < s_override_count; i++) {
        if (strcasecmp(s_test_overrides[i].key, key) == 0) {
            strncpy(target_buf, s_test_overrides[i].value, target_sz - 1);
            target_buf[target_sz - 1] = '\0';
            csilk_mutex_unlock(&s_secret_lock);
            return target_buf;
        }
        if (prefix[0] && strncasecmp(key, prefix, strlen(prefix)) == 0 &&
            key[strlen(prefix)] == '_' &&
            strcasecmp(s_test_overrides[i].key, key + strlen(prefix) + 1) == 0) {
            strncpy(target_buf, s_test_overrides[i].value, target_sz - 1);
            target_buf[target_sz - 1] = '\0';
            csilk_mutex_unlock(&s_secret_lock);
            return target_buf;
        }
    }

    /* 2. External Secret Manager */
    if (s_secret_manager) {
        if (s_secret_manager(key, target_buf, target_sz) == 0 && target_buf[0] != '\0') {
            csilk_mutex_unlock(&s_secret_lock);
            goto validate_and_return;
        }
    }
    csilk_mutex_unlock(&s_secret_lock);

    /* 3. Environment Provider */
    const char* env_val = getenv(key);
    if (!env_val && prefix[0]) {
        char full_env[128];
        snprintf(full_env, sizeof(full_env), "%s_%s", prefix, key);
        env_val = getenv(full_env);
    }
    if (!env_val && strncasecmp(key, "CSILK_", 6) != 0) {
        char csilk_env[128];
        snprintf(csilk_env, sizeof(csilk_env), "CSILK_%s", key);
        env_val = getenv(csilk_env);
    }
    if (!env_val && strncasecmp(key, "CSILK_", 6) == 0) {
        env_val = getenv(key + 6);
    }

    if (env_val && env_val[0] != '\0') {
        strncpy(target_buf, env_val, target_sz - 1);
        target_buf[target_sz - 1] = '\0';
        goto validate_and_return;
    }

    /* 4. File Provider */
    char file_env[128];
    snprintf(file_env, sizeof(file_env), "%s_FILE", key);
    const char* custom_file = getenv(file_env);
    if (!custom_file && prefix[0]) {
        snprintf(file_env, sizeof(file_env), "%s_%s_FILE", prefix, key);
        custom_file = getenv(file_env);
    }
    if (!custom_file && strncasecmp(key, "CSILK_", 6) != 0) {
        snprintf(file_env, sizeof(file_env), "CSILK_%s_FILE", key);
        custom_file = getenv(file_env);
    }
    if (custom_file && custom_file[0] != '\0') {
        if (read_secret_from_file(custom_file, target_buf, target_sz) == 0) {
            goto validate_and_return;
        }
    }

    /* Lowercase key for container secret paths */
    const char* pure_name = key;
    if (prefix[0] && strncasecmp(key, prefix, strlen(prefix)) == 0 && key[strlen(prefix)] == '_') {
        pure_name = key + strlen(prefix) + 1;
    } else if (strncasecmp(key, "CSILK_", 6) == 0) {
        pure_name = key + 6;
    }

    char   lower_name[64];
    size_t klen = strlen(pure_name);
    if (klen < sizeof(lower_name)) {
        for (size_t i = 0; i < klen; i++) {
            lower_name[i] = (char)tolower((unsigned char)pure_name[i]);
        }
        lower_name[klen] = '\0';

        char secret_path[256];
        /* /run/secrets/xxx (Docker / Kubernetes secret mounts) */
        snprintf(secret_path, sizeof(secret_path), "/run/secrets/%s", lower_name);
        if (read_secret_from_file(secret_path, target_buf, target_sz) == 0) {
            goto validate_and_return;
        }

        /* config/secrets/xxx (local file path) */
        snprintf(secret_path, sizeof(secret_path), "config/secrets/%s", lower_name);
        if (read_secret_from_file(secret_path, target_buf, target_sz) == 0) {
            goto validate_and_return;
        }
    }

    return NULL;

validate_and_return:
    /* Circuit breaker: reject default placeholder/weak secret in non-test mode */
    if (!is_test && contains_forbidden_pattern(target_buf)) {
        fprintf(stderr,
                "[CSILK SECURITY ERROR] Secret '%s' matched forbidden default pattern. "
                "Production execution refused! Please configure a secure secret.\n",
                key);
        target_buf[0] = '\0';
        return NULL;
    }

    return target_buf;
}

const char*
csilk_env_get(const char* key, char* out, size_t out_size, const char* default_val)
{
    if (!key || !key[0]) {
        return default_val;
    }

    char*  target_buf = out ? out : s_tls_env_ring[(s_tls_env_idx++) & 3];
    size_t target_sz = out ? out_size : sizeof(s_tls_env_ring[0]);
    target_buf[0] = '\0';

    const char* sec = csilk_secret_get(key, target_buf, target_sz);
    if (sec && sec[0] != '\0') {
        return sec;
    }

    ensure_lock_init();
    csilk_mutex_lock(&s_secret_lock);
    char prefix[64];
    strncpy(prefix, s_custom_prefix, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    csilk_mutex_unlock(&s_secret_lock);

    const char* val = getenv(key);
    if (!val && prefix[0]) {
        char full_env[128];
        snprintf(full_env, sizeof(full_env), "%s_%s", prefix, key);
        val = getenv(full_env);
    }
    if (!val && strncasecmp(key, "CSILK_", 6) != 0) {
        char csilk_env[128];
        snprintf(csilk_env, sizeof(csilk_env), "CSILK_%s", key);
        val = getenv(csilk_env);
    }

    if (val && val[0] != '\0') {
        strncpy(target_buf, val, target_sz - 1);
        target_buf[target_sz - 1] = '\0';
        return target_buf;
    }

    if (default_val) {
        strncpy(target_buf, default_val, target_sz - 1);
        target_buf[target_sz - 1] = '\0';
        return target_buf;
    }

    return NULL;
}

bool
csilk_secret_is_valid(const char* key)
{
    char        buf[128];
    const char* val = csilk_secret_get(key, buf, sizeof(buf));
    return (val != NULL && val[0] != '\0');
}
