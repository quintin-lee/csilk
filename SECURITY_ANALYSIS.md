# csilk Security Vulnerability Analysis Report

**Date**: 2026-08-25
**Scope**: Full source code audit across cryptographic, memory safety, input validation, and concurrency subsystems
**Method**: Static analysis of 40+ source files across 6 vulnerability categories
**Severity Distribution**: 1 Critical, 7 High, 9 Medium, 5 Low, 11 Informational

---

## Summary of Findings

| Severity | Count | Key Themes |
|:--------:|:-----:|---|
| **CRITICAL** | 1 | ~~JWT signature OOB read~~ ✅ **Fixed** |
| **HIGH** | 7 | ~~Zero-salt bcrypt~~ ✅ **Fixed**, ~~predictable CSRF tokens~~ ✅ **Fixed**, ~~weak nonce fallback~~ ✅ **Fixed**, ~~Redis injection~~ ✅ **Fixed**, ~~SQL injection (prepared stmts added)~~ ✅ **Mitigated**, ~~WebSocket key truncation~~ ✅ **Fixed**, ~~static path traversal~~ ✅ **Fixed** |
| **MEDIUM** | 9 | ~~Rate-limit saturation~~ ✅ **Fixed**, session fixation, ~~CORS credential leak~~ ✅ **Fixed**, ~~gzip expansion / bounds~~ ✅ **Fixed**, ~~hot-reload temp file TOCTOU~~ ✅ **Fixed** |
| **LOW** | 5 | Timing comparison fragility, unencrypted key material |
| **INFO** | 11 | Architectural observations (~~I1 DB pool timeout~~ ✅ **Fixed**) |

> **Update 2026-11-16**: Several findings have been fixed in the current codebase:
> C1 (JWT OOB), H1 (bcrypt zero salt), H2 (CSRF PRNG), H4 (Redis format-string), M1 (ratelimit saturation).
>
> **Update 2026-11-21**: Additional findings verified fixed in v1.1.1:
> H3 (nonce entropy failure now calls `abort()`), H5 (native prepared statements `csilk_stmt_prepare`/`csilk_stmt_bind_*` and `csilk_sql_rewrite_placeholders` added), H6 (WebSocket `WS_MAX_KEY_LEN` tightened to 128 with compile-time non-truncating buffer), H7 (static file `contains_path_traversal` fixed + `realpath()` canonical prefix check + `S_ISREG`), M4 (CORS wildcard origin rejected with `allow_credentials` in both single-origin and multi-origin list modes), M5 (gzip compression skips non-shrinking output and bounds `UINT_MAX`), M9 (hot-reload writes directly via `mkstemp` + `fdopen` + `fchmod(0700)` without close/reopen TOCTOU), and I1 (`csilk_db_pool_acquire_timeout` added).

---

## CRITICAL — Must Fix Immediately

### C1. JWT HS256 Signature Verification — Out-of-Bounds Read  ✅ **FIXED**
**File**: `src/middleware/jwt.c:347-363` (line numbers updated post-fix)
**Impact**: Authentication bypass or stack memory disclosure

> **Status**: Fixed. The current code uses `CRYPTO_memcmp` with a fixed-length comparison
> (`expected_len`), computing `len_mismatch` before the comparison to prevent
> timing side-channels. The `explicit_bzero(sig_actual, ...)` call ensures the
> HMAC buffer is wiped. See `jwt.c:342-379`.

```c
char sig_expected_b64[45];                    // 45-byte stack buffer
csilk_base64url_encode(sig_actual, 32, sig_expected_b64);
size_t sig_len = strlen(sig_ptr);             // attacker-controlled
sig_ok = (sig_len == strlen(sig_expected_b64)) &&
         (constant_time_compare(
              (const uint8_t*)sig_ptr,
              (const uint8_t*)sig_expected_b64,
              sig_len) == 0);                 // ← reads up to sig_len bytes
```

The `sig_len == strlen(sig_expected_b64)` check should prevent the OOB, but if an attacker crafts a token where `sig_ptr` points to a longer signature string (e.g., a malformed base64url token), and the comparison short-circuits due to compiler optimization or if `sig_expected_b64` is not NUL-terminated in all code paths, the `constant_time_compare` call with `sig_len > 44` reads past the buffer.

Additionally, `constant_time_compare` compares `a[i]` and `b[i]` for `i < len`. If `len > sizeof(sig_expected_b64)`, this reads adjacent stack variables.

**Evidence**: `sig_expected_b64` is exactly 45 bytes (32 bytes of base64url + NUL). The length check provides protection only if both sides are properly NUL-terminated and the check is not optimized away.

**Recommendation**:
```c
// Use fixed-length comparison with known sizes
sig_ok = (sig_len == 43) &&  // exact base64url length for 32 bytes
         (CRYPTO_memcmp((const uint8_t*)sig_ptr,
                        (const uint8_t*)sig_expected_b64,
                        43) == 0);
```

---

## HIGH — Fix in Next Release

### H1. bcrypt Salt Falls Back to All-Zeros on RNG Failure  ✅ **FIXED**
**File**: `src/crypto/bcrypt.c:380-391` (line numbers updated post-fix)
**Impact**: All password hashes become trivially crackable via rainbow tables

> **Status**: Fixed. When `RAND_bytes` and `RAND_priv_bytes` both fail, the
> code now calls `abort()` with a CWE-330 comment instead of silently falling
> back to a zeroed salt. The `TEST_OOM` path uses a deterministic zero salt
> for reproducibility in OOM tests.

```c
if (RAND_bytes(salt, sizeof(salt)) != 1) {
    if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
        memset(salt, 0, sizeof(salt));  // ← zero salt = rainbow table attack
    }
}
```

When entropy sources are exhausted (container environments, VMs without `/dev/urandom`), the salt becomes all zeros. A bcrypt hash with zero salt is reversible — the Eksblowfish key schedule with zero salt is deterministic and widely indexed in bcrypt hash databases.

**Fix**: Return error from `csilk_bcrypt_hash()` when RNG fails:
```c
if (RAND_bytes(salt, sizeof(salt)) != 1) {
    if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
        CSILK_LOG_E("bcrypt: RNG failure, cannot generate salt");
        return -1;
    }
}
```

### H2. CSRF Token Falls Back to Predictable PRNG  ✅ **FIXED**
**File**: `src/middleware/csrf.c:146-169` (line numbers updated post-fix)
**Impact**: Full CSRF protection bypass if `/dev/urandom` unavailable

> **Status**: Fixed. The predictable `rand_r(time() ^ getpid())` fallback has been
> removed. The current code logs a fatal error and aborts token generation
> when `/dev/urandom` cannot be opened (fail-closed).

```c
FILE* fp = fopen("/dev/urandom", "rb");
if (!fp) {
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    snprintf(buf, buf_size, "%08x%08x%08x%08x",
             rand_r(&seed), rand_r(&seed), rand_r(&seed), rand_r(&seed));
    // ← predictable: seed = time ^ pid, both observable
}
```

`rand_r(time() ^ getpid())` is trivially reproducible by an attacker who knows the process start time and PID. The token is then used for CSRF protection — an attacker can forge valid tokens.

**Fix**: Reject token generation when `/dev/urandom` is unavailable:
```c
if (!fp) {
    CSILK_LOG_E("CSRF: /dev/urandom unavailable, aborting token generation");
    return -1;
}
```

Alternatively, use the existing `csilk_crypto_fill_random()` function which has platform-appropriate CSPRNG fallbacks.

### H3. HMAC-SHA256 Nonce/Key — Weak Entropy Fallback  ✅ **FIXED**
**File**: `src/crypto/crypto.c:289-297`
**Impact**: Predictable nonces enable GCM forgery

> **Status**: Fixed. `csilk_crypto_generate_nonce()` now logs a fatal error and calls `abort()` immediately if `csilk_crypto_fill_random()` fails across all system CSPRNG sources, rather than falling back to a deterministic monotonic counter.

When `getrandom()`, `arc4random()`, `CryptGenRandom()`, and `/dev/urandom` all fail, the nonce falls back to a deterministic monotonic counter:
```c
static atomic_uintmax_t g_nonce_counter = 0;
uintmax_t counter = atomic_fetch_add(&g_nonce_counter, 1);
// nonce built from hrtime + counter
```

While this prevents nonce *reuse* (good), the output is predictable across reboots if the counter resets. Combined with H2 (CSRF token predictability), this indicates a systemic entropy handling issue.

**Fix**: Log fatal warning and abort; do not silently produce pseudo-random nonces.

### H4. Redis AUTH Format String Injection  ✅ **FIXED**
**File**: `src/drivers/db/redis.c:148` (line numbers updated post-fix)
**Impact**: Arbitrary command execution on Redis server

> **Status**: Fixed. `redisCommand(conn->c, "AUTH %s", password)` has been replaced
> with `redisCommandArgv(conn->c, 2, {"AUTH", password}, ...)` which passes the
> password as a separate argv element, eliminating any `%s` format-string
> interpretation. Raw SQL execution in `redis_exec_command_str` (line 320)
> remains by design — callers are expected to use parameterized commands.

### H5. All Database Drivers Accept Raw Unsanitized SQL  ✅ **MITIGATED**
**Files**: `src/drivers/db/sqlite.c`, `src/drivers/db/postgres.c`, `src/drivers/db/mysql.c`, `src/drivers/db/db.c`
**Impact**: SQL injection if callers concatenate untrusted strings into raw query APIs

> **Status**: Mitigated. Native prepared statements (`csilk_stmt_prepare`, `csilk_stmt_bind_*`, `csilk_stmt_step`) and parameterized query helpers (`csilk_db_query_param`, `csilk_db_exec_param`, `csilk_sql_rewrite_placeholders`, covered by `fuzz_sql_rewrite`) are now implemented across drivers. Raw `csilk_db_query` / `csilk_db_exec` remain available for static DDL/SQL by design.

```c
// sqlite.c:244
int rc = sqlite3_exec(conn->db, sql, NULL, NULL, &err);

// postgres.c:141
PGresult* pg_res = PQexec(conn->db, sql);

// mysql.c:237
if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
```

All three drivers accept raw SQL strings with zero sanitization, no parameterized queries, and no input validation. The framework's public API exposes `csilk_db_query(c, sql, ...)` which passes user input directly to these functions.

**Fix**: Add parameterized query support (`csilk_db_query_param`) with proper escaping for each driver.

### H6. WebSocket Handshake — Key Truncation / Buffer Sizing  ✅ **FIXED**
**File**: `src/protocols/websocket.c:46-95`
**Impact**: Oversized `Sec-WebSocket-Key` could truncate `WS_GUID` in `snprintf`

> **Status**: Fixed. `WS_MAX_KEY_LEN` is bounded to 128 bytes (rejecting empty or oversized keys with HTTP 400), and `combined[WS_MAX_KEY_LEN + sizeof(WS_GUID)]` is sized directly from the constants so `key || WS_GUID` can never be truncated.

```c
char combined[256];
snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);
csilk_sha1_update(&sha_ctx, (uint8_t*)combined, (uint32_t)strlen(combined));
```

While `snprintf` prevents writing past 256 bytes, `key` comes from `csilk_get_header(c, "Sec-WebSocket-Key")` which is attacker-controlled. If the header value exceeds ~220 bytes, `snprintf` truncates the output, but `strlen(combined)` returns the truncated length. The SHA-1 is computed on truncated data, producing an incorrect accept key. This doesn't cause a buffer overflow directly, but it causes the handshake to silently succeed with incorrect values, potentially allowing protocol confusion attacks.

**Actual risk**: If `csilk_get_header` returns a pointer into an arena buffer that gets reallocated, the `combined` buffer may reference stale data. More critically, if `key` is NULL (shouldn't happen after the check, but defensive coding), `snprintf` writes "(null)" causing unexpected behavior.

### H7. Path Traversal in Static File Middleware  ✅ **FIXED**
**File**: `src/middleware/static.c:200-275`
**Impact**: Arbitrary file read via symlink or path manipulation

> **Status**: Fixed. `contains_path_traversal()` properly checks both `prev` and `next` path separators, layered with `contains_null_or_control()`, `realpath()` canonical root-prefix verification, and `S_ISREG()` regular-file enforcement.

The `contains_path_traversal()` function has a duplicated check:
```c
// Bug: next == '\\0' checked twice instead of checking for '/' or end
while (*next) {
    if (*next == '/' || next == '\\0') {  // ← duplicated condition
        // ...
    }
    next++;
}
```

This allows bypassing path traversal detection via specific character sequences.

---

## MEDIUM — Address in Upcoming Sprint

### M1. Rate-Limit Table Saturation Bypass  ✅ **FIXED**
**File**: `src/middleware/ratelimit.c` (line numbers updated post-fix)
**Impact**: Rate limiting bypass with 1024+ distinct source IPs

> **Status**: Fixed. The original 1024-entry table has been expanded to 65536 entries
> (`MAX_IP_ENTRIES`). The current `get_or_create_ip_entry` returns `NULL` on saturation
> (rather than silently returning a shared slot), and the caller applies the configured
> `csilk_rate_saturation_policy_t` (evict-LRU, fail-open, or fail-closed).

### M2. Session Not Bound to Client Identity
**File**: `src/middleware/session.c`
**Impact**: Session hijacking via cookie theft

Session cookies are valid from any IP/User-Agent. A stolen session cookie provides full account access.

**Fix**: Add optional client binding via config flag `CSILK_SESSION_BIND_CLIENT`.

### M3. Session Value Lifetime Not Managed
**File**: `src/middleware/session.c:363-367`
**Impact**: Use-after-free via dangling pointers

```c
csilk_session_set(c, key, value_ptr);  // stores raw void*
```

Callers may free memory while the session is still active.

**Fix**: Require cleanup callback or deep-copy values.

### M4. CORS Wildcard Origin with Credentials  ✅ **FIXED**
**File**: `src/middleware/cors.c:68-95`
**Impact**: Cross-origin credential theft

> **Status**: Fixed. Both single-origin (`allow_origin`) and multi-origin (`allow_origin_list`) modes now reject wildcard `"*"` when `allow_credentials` is enabled, omitting `Access-Control-Allow-Origin` and logging a warning.

When `AllowCredentials` is enabled, wildcard `*` origin violates RFC 6454 and allows any site to make credentialed requests.

**Fix**: Reject wildcard origin when credentials are enabled.

### M5. Gzip Bomb / Incompressible Expansion  ✅ **FIXED**
**File**: `src/middleware/gzip.c:45-108, 233-280`
**Impact**: Bandwidth/CPU waste on incompressible payloads and `uInt` bounds

> **Status**: Fixed. Input bodies exceeding `UINT_MAX` are rejected before `uInt` cast, allocation uses `deflateBound()` directly without overflowing `src_len * 10`, and both synchronous and asynchronous deflate paths discard the compressed buffer whenever `compressed_len >= src_len`.

`deflateBound()` allocation has no upper limit. A 1KB input can expand to GBs of output.

**Fix**: Enforce compression ratio limit (e.g., max 10:1 expansion).

### M6. URL Decode — Invalid Sequences Passed Through
**File**: `src/core/primitives/url.c`
**Impact**: Encoding evasion of WAF rules

Invalid `%XX` sequences are passed through unchanged instead of being rejected or replaced.

**Fix**: Reject or normalize invalid percent-encoding.

### M7. Config Validation Gaps
**File**: `src/core/config/config.c`
**Impact**: Misconfiguration leading to security issues

- `atoi()`/`atoll()` used without range checks on port values
- Negative timeouts accepted
- `base_url` not validated for protocol or path traversal
- YAML depth unbounded

**Fix**: Add validation layer with explicit ranges.

### M8. Mass Assignment via Reflection
**File**: `src/reflection/reflect_marshal.c`
**Impact**: Privilege escalation via untrusted JSON input

`csilk_json_marshal` serializes all struct fields without allowlist/denylist.

**Fix**: Add field-level access control via annotation or explicit allowlist.

### M9. Hot Reload Temp File Race  ✅ **FIXED**
**File**: `src/core/config/hot_reload.c:87-146`
**Impact**: TOCTOU on temp file creation

> **Status**: Fixed. `create_temp_lib_copy()` now streams the source shared library directly into the file descriptor returned by `mkstemp()` via `fdopen(dst_fd, "wb")` and sets owner-only permissions with `fchmod(fileno(out), 0700)` before closing, eliminating the close-then-reopen path race and world-readable `/tmp` permissions.

```c
int fd = mkstemp(out_path);  // predictable temp name
// ...
unlink(out_path);           // race window between mkstemp and unlink
```

Between `mkstemp()` and `unlink()`, another process could create a file with the same name.

**Fix**: Use `O_EXCL` flag in custom `open()` or use a more secure temp directory.

---

## LOW — Documentation/Minor Fixes

### L1. `constant_time_compare` Implementation Fragile
**File**: `src/middleware/jwt.c:52-60`
```c
volatile uint8_t diff = 0;  // compiler may still optimize
```
Use OpenSSL's `CRYPTO_memcmp()` consistently.

### L2. RSA Private Keys Not Zeroed
**File**: `src/drivers/cipher/openssl.c`
Private key material not explicitly zeroed before free.

### L3. JWT Algorithm Default Case
**File**: `src/middleware/jwt.c:21-30`
Unknown algorithm enum returns `HS256` string, silently degrading.

### L4. HMAC Accepts NULL Key/Data
**File**: `src/crypto/crypto.c`
NULL key → empty string; NULL data → empty string. No validation.

### L5. Nonce Predictable After Reboot
**File**: `src/crypto/crypto.c:290-300`
Deterministic counter-based nonce if CSPRNG unavailable.

---

## INFORMATIONAL — Architectural Observations

| ID | Observation | Impact |
|---|---|---|
| I1 | ~~No connection pool timeout~~ ✅ **Fixed** (`csilk_db_pool_acquire_timeout` added); DB drivers remain synchronous C clients | Long queries should be offloaded if needed |
| I2 | No request body size enforcement during streaming (only checked after parse) | Slow-loris DoS possible |
| I3 | No CRLF validation on HTTP headers | Potential response splitting |
| I4 | WAF pattern matching is case-insensitive but doesn't handle percent-encoding | Encoding evasion possible |
| I5 | AI driver `base_url` has no allowlist/blocklist | SSRF to internal services |
| I6 | Lock-free queue uses `memory_order_relaxed` for some operations | Potential ordering issues on ARM |
| I7 | `volatile` used for mutex-protected indices in thread pool | Misleading; mutex already provides synchronization |
| I8 | No rate limiting on admin endpoints | Admin panel accessible without throttling |
| I9 | Server shutdown doesn't synchronize active_clients iteration | TOCTOU on client list during drain |
| I10 | RCU epoch advancement happens before publication | Race window for stale router access |
| I11 | Global distributed workflow registry unprotected | Concurrent registration race |

---

## Remediation Priority Matrix

| Priority | Issues | Effort | Timeline |
|---|---|---|---|
| **P0** | ~~C1 (JWT OOB)~~ ✅ Fixed | — | — |
| **P1** | ~~H1, H2, H3, H4, H6, H7~~ ✅ Fixed, ~~H5~~ ✅ Mitigated (prepared statements + doc warning) | — | Completed |
| **P2** | ~~M1, M4, M5, M9, I1~~ ✅ Fixed | — | Completed |
| **P3** | M2, M3, M6-M8 (session binding/lifetime, URL decode policy, config/reflection allowlists) | 8h | Q4 roadmap |
| **P4** | L1-L5, I2-I11 (Hardening, docs) | 16h | Ongoing |

---

## Verification Commands

```bash
# Run ASAN to detect memory safety issues
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
ctest --test-dir build -E test_integration --timeout 30

# Run TSAN to detect race conditions
TSAN_OPTIONS="halt_on_error=1" \
ctest --test-dir build -E test_integration --timeout 30

# Fuzz test coverage
./fuzz_test -max_total_time=120 fuzz/corpus/fuzz_test
./fuzz_url -max_total_time=60 fuzz/corpus/fuzz_url
```
