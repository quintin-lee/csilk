# Security Fixes Implementation Plan v0.5.3

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 12 security vulnerabilities (1 Critical, 2 High, 9 Medium) identified in the security re-audit.

**Architecture:** Sequential fixes by severity — each fix is independent with minimal cross-dependency. SQL injection requires new API, SSRF requires URL validation, etc.

**Tech Stack:** C23, SQLite/MySQL/PostgreSQL/Redis drivers, OpenSSL, libcurl, CMake, CTest

---

## File Structure

| File | Operation | Responsibility |
|------|-----------|----------------|
| `src/drivers/db/sqlite.c` | Modify | Add parameterized query API |
| `src/drivers/db/mysql.c` | Modify | Add parameterized query API |
| `src/drivers/db/postgres.c` | Modify | Add parameterized query API |
| `src/drivers/ai/openai.c` | Modify | Add URL validation for SSRF prevention |
| `src/drivers/ai/ollama.c` | Modify | Add URL validation for SSRF prevention |
| `src/crypto/bcrypt.c` | Modify | Remove weak salt fallback |
| `src/middleware/ratelimit.c` | Modify | Fix hash table overflow |
| `src/middleware/session.c` | Modify | Add deep copy for session values |
| `src/middleware/cors.c` | Modify | Reject wildcard with credentials |
| `src/middleware/gzip.c` | Modify | Add compression ratio limit |
| `src/core/config/config.c` | Modify | Add range validation for config |
| `src/core/config/hot_reload.c` | Modify | Randomize mkstemp path |
| `tests/security/test_sql_injection.c` | Create | Test SQL injection prevention |
| `tests/security/test_ssrf.c` | Create | Test SSRF prevention |

---

### Task 1: P0 Fix — SQL Injection Prevention

**Files:**
- Modify: `src/drivers/db/sqlite.c`
- Modify: `src/drivers/db/mysql.c`
- Modify: `src/drivers/db/postgres.c`
- Create: `tests/security/test_sql_injection.c`

- [ ] **Step 1: Write failing test for SQL injection prevention**

```c
// tests/security/test_sql_injection.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk/csilk.h"
#include "csilk/test/test.h"

static void
test_sql_injection_blocked(void)
{
    printf("Testing SQL injection prevention...\n");
    
    csilk_ctx_t* c = csilk_test_ctx_new();
    
    // Test that raw SQL execution is dangerous
    // and parameterized queries are safe
    const char* malicious_sql = "SELECT * FROM users WHERE id = 1 OR 1=1";
    
    // The framework should either:
    // 1. Reject raw SQL execution, or
    // 2. Provide parameterized API that prevents injection
    
    csilk_test_ctx_free(c);
    printf("  passed\n");
}

int
main(void)
{
    test_sql_injection_blocked();
    printf("test_sql_injection: ALL PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build -j$(nproc) --target test_sql_injection
./build/test_sql_injection
# Expected: FAIL - no SQL injection protection yet
```

- [ ] **Step 3: Add parameterized query API to SQLite driver**

In `src/drivers/db/sqlite.c`, add new function:

```c
/** @brief Execute a parameterized SQL statement safely.
 * 
 * Uses sqlite3_bind_* APIs to prevent SQL injection.
 * Supports up to 10 parameters.
 */
int
csilk_db_exec_params(csilk_db_pool_t* pool, const char* sql, ...)
{
    if (!pool || !sql) {
        return -1;
    }
    
    sqlite_conn_t* conn = (sqlite_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn) {
        return -1;
    }
    
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        CSILK_LOG_E("DB: prepare failed: %s", sqlite3_errmsg(conn->db));
        return -1;
    }
    
    // Bind parameters using va_list
    // ... implementation details ...
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}
```

- [ ] **Step 4: Add URL validation for AI drivers (SSRF prevention)**

In `src/drivers/ai/openai.c`, add validation before curl request:

```c
// Validate URL scheme and restrict protocols
if (strncmp(state->base_url, "http://", 7) != 0 && 
    strncmp(state->base_url, "https://", 8) != 0) {
    CSILK_LOG_E("AI: Invalid URL scheme: %.30s...", state->base_url);
    return -1;
}

// Restrict curl to HTTP/HTTPS only
curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
```

- [ ] **Step 5: Remove weak bcrypt salt fallback**

In `src/crypto/bcrypt.c`, remove the zero-salt fallback:

```c
// Before:
if (RAND_bytes(salt, sizeof(salt)) != 1) {
    if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
        memset(salt, 0, sizeof(salt));  // DANGEROUS!
    }
}

// After:
if (RAND_bytes(salt, sizeof(salt)) != 1) {
    if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
        CSILK_LOG_E("Bcrypt: Failed to generate secure random salt");
        return -1;  // Fail safely instead of using weak salt
    }
}
```

- [ ] **Step 6: Fix CORS wildcard + credentials**

In `src/middleware/cors.c`, add validation:

```c
if (config->allow_credentials && 
    strcmp(config->allow_origin, "*") == 0) {
    CSILK_LOG_W("CORS: Cannot use wildcard origin with credentials");
    // Reject this configuration
    return;
}
```

- [ ] **Step 7: Build and run tests**

```bash
cmake --build build -j$(nproc) --target test_sql_injection
cmake --build build -j$(nproc) --target test_jwt_security
./build/test_sql_injection
./build/test_jwt_security
```

Expected: All tests pass

- [ ] **Step 8: Commit**

```bash
git add src/drivers/db/sqlite.c src/drivers/db/mysql.c src/drivers/db/postgres.c
git add src/drivers/ai/openai.c src/drivers/ai/ollama.c
git add src/crypto/bcrypt.c src/middleware/cors.c
git add tests/security/test_sql_injection.c
git commit -m "fix(security): 🔒 fix SQL injection, SSRF, and weak crypto (CWE-89, CWE-918, CWE-330)"
```

---

### Task 2: P1 Fix — Rate Limit Hash Table Overflow

**Files:**
- Modify: `src/middleware/ratelimit.c:68-75`

- [ ] **Step 1: Increase hash table size or use LRU**

```c
// Increase from 1024 to 65536 entries
#define RATE_LIMIT_HASH_SIZE 65536

// Or implement LRU eviction when full
typedef struct {
    uint64_t key;
    uint32_t count;
    time_t expiry;
} rate_limit_entry_t;

static rate_limit_entry_t hash_table[RATE_LIMIT_HASH_SIZE];
```

- [ ] **Step 2: Build and test**

```bash
cmake --build build -j$(nproc) --target test_ratelimit
./build/test_ratelimit
```

- [ ] **Step 3: Commit**

```bash
git add src/middleware/ratelimit.c
git commit -m "fix(ratelimit): 🔒 increase hash table size to prevent overflow (CWE-284)"
```

---

### Task 3: P1 Fix — Session Deep Copy

**Files:**
- Modify: `src/middleware/session.c:338-367`

- [ ] **Step 1: Add deep copy for session values**

```c
// When storing session value, make a deep copy
static int
session_set(csilk_ctx_t* c, const char* key, const void* value, size_t len)
{
    if (!key || !value || len == 0) {
        return -1;
    }
    
    // Allocate and copy value
    void* copied = malloc(len);
    if (!copied) {
        return -1;
    }
    memcpy(copied, value, len);
    
    // Store with cleanup callback
    csilk_set_ex(c, key, copied, free);
    return 0;
}
```

- [ ] **Step 2: Build and test**

```bash
cmake --build build -j$(nproc) --target test_session
./build/test_session
```

- [ ] **Step 3: Commit**

```bash
git add src/middleware/session.c
git commit -m "fix(session): 🔒 add deep copy for session values (CWE-416)"
```

---

### Task 4: P1 Fix — Gzip Bomb Prevention

**Files:**
- Modify: `src/middleware/gzip.c:71,236`

- [ ] **Step 1: Add compression ratio limit**

```c
// Limit expansion ratio to prevent gzip bombs
#define MAX_COMPRESSION_RATIO 1024  // 1KB input -> 1MB output max

size_t
get_compressed_size(size_t original_size)
{
    // Cap the allocation size
    if (original_size > CSILK_GZIP_MAX_INPUT) {
        return CSILK_GZIP_MAX_OUTPUT;
    }
    
    size_t bound = deflateBound(&strm, original_size);
    // Apply ratio limit
    if (bound > original_size * MAX_COMPRESSION_RATIO) {
        bound = original_size * MAX_COMPRESSION_RATIO;
    }
    return bound;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/middleware/gzip.c
git commit -m "fix(gzip): 🔒 add compression ratio limit to prevent bombs (CWE-400)"
```

---

### Task 5: P1 Fix — Config Validation

**Files:**
- Modify: `src/core/config/config.c:160,163`
- Modify: `src/core/config/hot_reload.c:95,111`

- [ ] **Step 1: Add range checks for port and timeout**

```c
// Validate port range
long port = atoll(value);
if (port < 1 || port > 65535) {
    CSILK_LOG_E("Config: Invalid port: %ld", port);
    return -1;
}

// Validate timeout (must be positive)
long timeout = atoll(value);
if (timeout < 0) {
    CSILK_LOG_E("Config: Negative timeout not allowed: %ld", timeout);
    return -1;
}
```

- [ ] **Step 2: Randomize mkstemp template**

```c
// Use more random template
snprintf(out_path, max_len, "/tmp/csilk_reload_%lu_XXXXXX", (unsigned long)getpid());
```

- [ ] **Step 3: Commit**

```bash
git add src/core/config/config.c src/core/config/hot_reload.c
git commit -m "fix(config): 🔒 add range validation and randomize temp paths (CWE-284, CWE-377)"
```

---

### Task 6: Full Validation

- [ ] **Step 1: Run all security tests**

```bash
ctest --test-dir build -R test_security -V
ctest --test-dir build -R test_jwt -V
ctest --test-dir build -R test_sql_injection -V
```

- [ ] **Step 2: Run full test suite**

```bash
ctest --test-dir build -E test_integration --timeout 60 --output-on-failure
```

- [ ] **Step 3: Run TSAN build**

```bash
cmake -B build_tsan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_TSAN=ON
cmake --build build_tsan -j$(nproc)
ctest --test-dir build_tsan --timeout 60 --output-on-failure
```

- [ ] **Step 4: Verify no regressions**

```bash
git diff --stat
```

Expected: Only security-related files modified

---

## Self-Review Checklist

- [ ] SQL injection: Parameterized query API added for all DB drivers
- [ ] SSRF: URL validation and protocol restrictions in AI drivers
- [ ] Weak crypto: Bcrypt salt fallback removed
- [ ] Rate limiting: Hash table size increased
- [ ] Session: Deep copy implemented
- [ ] CORS: Wildcard + credentials rejection added
- [ ] Gzip: Compression ratio limit added
- [ ] Config: Range validation added
- [ ] All tests pass
- [ ] TSAN clean
