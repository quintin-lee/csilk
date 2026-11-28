# csilk Security Vulnerability Analysis Report

**Date**: 2026-08-25 (Re-verified: 2026-11-27 for v1.2.0)
**Scope**: Full source code audit across cryptographic, memory safety, input validation, and concurrency subsystems
**Method**: Static analysis of 40+ source files across 6 vulnerability categories
**Severity Distribution**: 1 Critical, 7 High, 9 Medium, 5 Low, 11 Informational

---

## Summary of Findings

| Severity | Count | Key Themes |
|:--------:|:-----:|---|
| **CRITICAL** | 1 | ~~JWT signature OOB read~~ ✅ **Fixed** |
| **HIGH** | 7 | ~~Zero-salt bcrypt~~ ✅ **Fixed**, ~~predictable CSRF tokens~~ ✅ **Fixed**, ~~weak nonce fallback~~ ✅ **Fixed**, ~~Redis injection~~ ✅ **Fixed**, ~~SQL injection (prepared stmts added)~~ ✅ **Mitigated**, ~~WebSocket key truncation~~ ✅ **Fixed**, ~~static path traversal~~ ✅ **Fixed** |
| **MEDIUM** | 9 | ~~Rate-limit saturation~~ ✅ **Fixed**, session binding (open), ~~session value lifetime APIs~~ ✅ **Mitigated**, ~~CORS credential leak~~ ✅ **Fixed**, ~~gzip expansion / bounds~~ ✅ **Fixed**, ~~URL invalid `%XX` escaping~~ ✅ **Fixed**, ~~config validation~~ ✅ **Fixed**, reflection mass assignment (open), ~~hot-reload temp file TOCTOU~~ ✅ **Fixed** |
| **LOW** | 5 | `constant_time_compare` vs `CRYPTO_memcmp`, key buffer cleansing, ~~JWT unknown alg label~~ ✅ **Fixed**, HMAC NULL acceptance, ~~nonce fallback~~ ✅ **Fixed** |
| **INFO** | 11 | ~~I1 DB pool timeout~~, ~~I2 streaming body limit~~, ~~I3 CRLF sanitization~~, ~~I5 base_url SSRF check~~, ~~I7 `volatile` removal~~, ~~I10 RCU swap order~~, ~~I11 `g_dist_mutex`~~ ✅ **Fixed**; ~~I6/I9~~ 📝 **Verified Safe / By Design**; ~~I8 admin auth~~ ⚠️ **Partial**; I4 open |

> **Update 2026-11-16**: Several findings have been fixed in the current codebase:
> C1 (JWT OOB), H1 (bcrypt zero salt), H2 (CSRF PRNG), H4 (Redis format-string), M1 (ratelimit saturation).
>
> **Update 2026-11-21**: Additional findings verified fixed in v1.1.1:
> H3 (nonce entropy failure now calls `abort()`), H5 (native prepared statements `csilk_stmt_prepare`/`csilk_stmt_bind_*` and `csilk_sql_rewrite_placeholders` added), H6 (WebSocket `WS_MAX_KEY_LEN` tightened to 128 with compile-time non-truncating buffer), H7 (static file `contains_path_traversal` fixed + `realpath()` canonical prefix check + `S_ISREG`), M4 (CORS wildcard origin rejected with `allow_credentials` in both single-origin and multi-origin list modes), M5 (gzip compression skips non-shrinking output and bounds `UINT_MAX`), M9 (hot-reload writes directly via `mkstemp` + `fdopen` + `fchmod(0700)` without close/reopen TOCTOU), and I1 (`csilk_db_pool_acquire_timeout` added).
>
> **Update 2026-11-27 (v1.2.0 re-verification)**: Synchronized remaining statuses with `master`:
> M3 mitigated via `csilk_session_set_ex()` / `csilk_session_set_len()`, M6 fixed via `%25` normalization in `url.c`, M7 partially mitigated via `csilk_config_validate()`, L3 fixed (`"unknown algorithm"`), L5 deduplicated with H3 (`abort()`), I2 fixed in `http1_parse.c:on_body()`, I3 fixed in `header_map.c` / `response.c` (CWE-113), I6/I9/I10 verified safe or fixed in `lfqueue.h` / `server_shutdown.c` / `server_rcu.c`, I7 obsolete (no remaining `volatile` indices), I8 partially mitigated via mandatory auth in `csilk_admin_serve_secure()`, and I11 fixed with `g_dist_mutex`.

---

## CRITICAL — Must Fix Immediately

### C1. JWT HS256 Signature Verification — Out-of-Bounds Read  ✅ **FIXED**
**File**: `src/middleware/jwt.c:342-358` (line numbers updated post-fix)
**Impact**: Authentication bypass or stack memory disclosure

> **Status**: Fixed. `jwt.c:352-358` now bounds signature length with `strnlen(sig_ptr, 45)`, copies up to 44 bytes into a zero-initialized `uint8_t sig_actual_b64[45]`, evaluates `(sig_len == 43) & (constant_time_compare(sig_actual_b64, (const uint8_t*)sig_expected_b64, 44) == 0)` without short-circuit branching, and wipes `sig_actual` via `explicit_bzero()`. See L1 regarding replacing `constant_time_compare()` with `CRYPTO_memcmp()`.

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

### M2. Session Not Bound to Client Identity  ✅ **MITIGATED**
**File**: `src/middleware/session.c`
**Impact**: Session hijacking via cookie theft

> **Status**: Mitigated (opt-in). `csilk_session_set_bind_client(1)` enables per-session SHA-256 binding of client IP + `User-Agent`, verifying with constant-time `CRYPTO_memcmp()` on session resume and rotating mismatched sessions.

**Fix**: Enable client binding with `csilk_session_set_bind_client(1)`.

### M3. Session Value Lifetime Not Managed  ✅ **MITIGATED**
**File**: `src/middleware/session.c:677-715`
**Impact**: Use-after-free via dangling pointers

> **Status**: Mitigated. `csilk_session_set_ex()` (custom cleanup destructor) and `csilk_session_set_len()` (bounded value copy + destructor) are now available for safe lifetime management; `csilk_session_set()` remains a zero-length raw-pointer convenience wrapper.

```c
csilk_session_set(c, key, value_ptr);  // stores raw void* (no destructor)
```

**Recommendation**: Prefer `csilk_session_set_ex()` or `csilk_session_set_len()` for heap/owned values.

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

### M6. URL Decode — Invalid Sequences Passed Through  ✅ **FIXED**
**File**: `src/core/primitives/url.c:44-90`
**Impact**: Encoding evasion of WAF rules

> **Status**: Fixed. `csilk_url_decode()` now normalizes invalid `%` sequences by escaping `'%'` as `"%25"` (CWE-116 / RFC 3986 §2.1) so downstream WAF checks operate on canonicalized output.

Invalid `%XX` sequences are passed through unchanged instead of being rejected or replaced.

**Fix**: Reject or normalize invalid percent-encoding.

### M7. Config Validation Gaps  ✅ **FIXED**
**File**: `src/core/config/config.c`
**Impact**: Misconfiguration leading to security issues

> **Status**: Fixed. YAML integer parsing now uses bounded `strtol()`/`strtoll()` helpers (`parse_int_strtol`, `parse_ll_clamped`, `parse_timeout_ms`), string updates free prior values on duplicate keys, YAML nesting depth is capped at `CONFIG_YAML_MAX_DEPTH` (32), and `csilk_config_validate()` checks `idle_timeout_ms`/`read_timeout_ms`/`write_timeout_ms`/`request_timeout_ms` range (`<= INT_MAX`) plus `ai.base_url` scheme/SSRF safety via `csilk_security_url_is_ssrf_safe()`.

### M8. Mass Assignment via Reflection  ✅ **MITIGATED**
**File**: `src/reflection/reflect_unmarshal.c`
**Impact**: Privilege escalation via untrusted JSON input

> **Status**: Mitigated. `csilk_json_unmarshal_allowlisted()` restricts top-level JSON unmarshalling to an explicit field allowlist (`allowed_fields` / `allowed_count`).

**Fix**: Use `csilk_json_unmarshal_allowlisted()` for untrusted payloads.

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

### L1. `constant_time_compare` Implementation Fragile  ✅ **FIXED**
**File**: `src/middleware/jwt.c:340`

> **Status**: Fixed. Removed custom `constant_time_compare()`; HS256 JWT verification uses OpenSSL `CRYPTO_memcmp()`.

### L2. RSA Private Keys Not Zeroed  ✅ **FIXED**
**File**: `src/crypto/cipher_openssl.c:213-295`

> **Status**: Fixed. `cleanse_and_free_bio()` wipes `priv_bio`'s internal `BUF_MEM` buffer with `OPENSSL_cleanse()` before `BIO_free()` across all exit paths of `default_generate_keypair()`.

### L3. JWT Algorithm Default Case  ✅ **FIXED**
**File**: `src/middleware/jwt.c:27-39`

> **Status**: Fixed. `jwt_alg_str()` now returns `"unknown algorithm"` in the `default:` case instead of degrading to `"HS256"`.

### L4. HMAC Accepts NULL Key/Data  ✅ **FIXED**
**File**: `src/crypto/crypto.c:89-105`

> **Status**: Fixed. `csilk_hmac_sha256()` now zeroes `out` and returns early when `key`/`data` is `NULL` with a non-zero length (preventing OOB reads) or `key_len > INT_MAX`, while preserving valid zero-length `(NULL, 0)` behavior.

### L5. Nonce Predictable After Reboot  ✅ **FIXED** (covered by H3)
**File**: `src/crypto/crypto.c:289-297`

> **Status**: Fixed alongside H3. `csilk_crypto_generate_nonce()` calls `abort()` when CSPRNG sources fail; the deterministic counter fallback has been removed.

---

## INFORMATIONAL — Architectural Observations

| ID | Observation | Impact / Current Status (v1.2.0) |
|---|---|---|
| I1 | ~~No connection pool timeout~~ ✅ **Fixed** (`csilk_db_pool_acquire_timeout` added); DB drivers remain synchronous C clients | Long queries should be offloaded if needed |
| I2 | ~~No request body size enforcement during streaming~~ ✅ **Fixed** (`http1_parse.c:on_body()` enforces `max_body_size` incrementally and returns `HPE_USER`) | Slow-loris / oversized body capped mid-stream |
| I3 | ~~No CRLF validation on HTTP headers~~ ✅ **Fixed** (`header_map.c:506-528` & `response.c:135` strip CR/LF, CWE-113) | Response splitting prevented |
| I4 | WAF pattern matching (`waf.c`) is case-insensitive with scan budgets (`256 KB` body / `64 KB` value) and inspects `url.c`-canonicalized `%25`/decoded input, but does not perform extra multi-round unescaping | Double-encoding evasion remains policy/caller scope |
| I5 | ~~`ssrf.c` provides `csilk_security_url_is_ssrf_safe()`, not yet wired into AI driver `base_url` validation~~ ✅ **Fixed** | Validated in `csilk_config_validate()` and enforced across AI/Vector/MCP HTTP drivers via `drivers/curl_common.h` |
| I6 | 📝 **Verified Safe**: `lfqueue.h` `relaxed` store on producer `next` precedes `acq_rel` head exchange and `release` `prev->next` link, paired with `acquire` loads on consumer (Vyukov MPSC) | Ordering is sound on ARM/x86 |
| I7 | ~~`volatile` used for mutex-protected indices in thread pool~~ ✅ **Fixed / Obsolete** | No `volatile` index fields remain in `src/core/` |
| I8 | ⚠️ **Partially Mitigated**: `csilk_admin_serve()` fails closed and `csilk_admin_serve_secure()` (`admin.c:478-501`) requires non-NULL `auth_middleware` | Auth enforced; rate limiting remains caller opt-in |
| I9 | 📝 **By Design**: `close_active_clients()` (`server_shutdown.c:40-92`) traverses `wp->active_clients` strictly on the owning worker event loop | Thread-confined; no cross-thread lock needed |
| I10 | ~~RCU epoch advancement happens before publication~~ ✅ **Fixed / Verified Safe** (`server_rcu.c:467-470` swaps `server->router` before advancing `global_epoch`, paired with `seq_cst` reader acquire fence) | No stale router race window |
| I11 | ~~Global distributed workflow registry unprotected~~ ✅ **Fixed** (`wf_distributed.c:18-186` guarded by `g_dist_mutex`) | Thread-safe registration/lookup |

---

## Remediation Priority Matrix

| Priority | Issues | Effort | Timeline |
|---|---|---|---|
| **P0** | ~~C1 (JWT OOB)~~ ✅ Fixed | — | Completed |
| **P1** | ~~H1, H2, H3, H4, H6, H7~~ ✅ Fixed, ~~H5~~ ✅ Mitigated (prepared statements + doc warning) | — | Completed |
| **P2** | ~~M1, M4, M5, M6, M9, I1, I2, I3, I7, I10, I11~~ ✅ Fixed, ~~M3~~ ✅ Mitigated | — | Completed |
| **P3** | ~~M7~~ ✅ Fixed, ~~M2, M8~~ ✅ Mitigated (opt-in session client binding & reflection allowlist) | Completed | Completed |
| **P4** | ~~I5, L1, L2, L4~~ ✅ Fixed, I4, I8 (Hardening, docs) | Ongoing | Ongoing |

---

## Verification Commands

```bash
# Run ASAN to detect memory safety issues (Full build)
ASAN_OPTIONS="detect_leaks=1:symbolize=1:abort_on_error=1" \
LSAN_OPTIONS="suppressions=$PWD/cmake/.lsan-suppressions" \
ctest --test-dir build_full --timeout 180 --output-on-failure

# Run TSAN to detect race conditions
TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/cmake/.tsan-suppressions" \
ctest --test-dir build_tsan -E test_integration --timeout 180 --output-on-failure

# Fuzz test coverage (configure with -DCMAKE_C_COMPILER=clang -DCSILK_BUILD_FULL=ON -DUSE_ASAN=ON -DUSE_FUZZER=ON)
./build_fuzz/fuzz_test -max_total_time=120 -dict=fuzz/fuzz.dict fuzz/corpus/fuzz_test
./build_fuzz/fuzz_url -max_total_time=60 -dict=fuzz/fuzz.dict fuzz/corpus/fuzz_url
```
