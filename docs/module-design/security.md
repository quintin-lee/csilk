# Security / Permissions Module

## 1. Overview

The Security module provides a **layered security architecture** spanning authentication (JWT), authorization (pluggable permission/ACL drivers), request validation (WAF, CSRF), and transport-level protections (CORS, rate limiting). Each layer is implemented as middleware and can be independently enabled, configured, or replaced. Security middleware **MUST** be evaluated in the documented order: CORS → Rate Limiter → WAF → CSRF → JWT → Permission. Rate limiter **MUST** use sliding-window counters with atomic increments. JWT verification **MUST NOT** allocate on the hot path for valid tokens. WAF rules **SHOULD** be compiled to a deterministic finite automaton (DFA) for O(n) scanning with input length n.

**Files**: `src/security/perm.c`, `src/middleware/jwt.c`, `src/middleware/waf.c`, `src/middleware/csrf.c`, `src/middleware/cors.c`, `include/csilk/drivers/perm.h`

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Request Pipeline                       │
│                                                          │
│  ┌──────────┐  ┌──────┐  ┌─────────┐  ┌─────────────┐  │
│  │ CORS     │→│ Rate │→│ JWT     │→│ CSRF        │  │
│  │ MW       │  │Limit │  │ Auth MW │  │ Protection  │  │
│  └──────────┘  └──────┘  └─────────┘  └─────────────┘  │
│                                      │                  │
│                                      ▼                  │
│  ┌──────────────┐  ┌────────────┐  ┌──────────────┐    │
│  │ WAF          │→│ Permission │→│ Route        │    │
│  │ (SQLi/XSS/   │  │ Auto-MW    │  │ Handler      │    │
│  │  Path Trav.) │  │ (RBAC)     │  │              │    │
│  └──────────────┘  └────────────┘  └──────────────┘    │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │              Permission Drivers                   │   │
│  │  "simple" (built-in RBAC) │ custom (via plugin)  │   │
│  │  csilk_perm_driver_t.check(ctx, perm, resource)  │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **Pluggable permission driver** — The authorization layer uses a single-function vtable (`check(ctx, permission, resource)` → 0/≠0). The built-in "simple" driver provides in-memory RBAC; custom drivers can implement Casbin, OPA, or any other policy engine.

2. **Route-level permission metadata** — Routes can declare required permissions and resources at registration time via `csilk_app_add_route_extended_perm()`. The `csilk_perm_auto_middleware()` reads this metadata and enforces it automatically — no per-route boilerplate.

3. **JWT-driven identity** — The JWT middleware extracts and verifies Bearer tokens, stores the decoded payload (including `role`) on the request context, and provides the identity foundation for downstream permission checks.

4. **Layered, not monolithic** — Authentication, authorization, CSRF, WAF, CORS, and rate limiting are separate middleware modules that compose in the handler chain. Each can be independently enabled, configured, or omitted.

---

## 3. Permission System

### 3.1 Driver Interface

```c
// include/csilk/drivers/perm.h
struct csilk_perm_driver_s {
    const char* name;
    int (*check)(csilk_ctx_t* c, const char* permission, const char* resource);
};
```

A permission driver is a named vtable with a single `check()` callback. The callback receives the request context (identity), the required permission, and the target resource, and returns 0 if allowed, non-zero if denied.

### 3.2 Global Registry

```c
static csilk_perm_driver_t* drivers[16];   // Fixed-size registry
static csilk_perm_driver_t* default_driver; // Active driver for checks
static atomic_int perm_initialized;
```

| Function | Role |
|---|---|
| `csilk_perm_init()` | Atomic CAS-once init → registers "simple" driver |
| `csilk_perm_register_driver(name, vtable)` | Add driver to registry (max 16), first becomes default |
| `csilk_perm_get_driver(name)` | Linear search by name |
| `csilk_perm_set_default(name)` | Switch active driver |

The first registered driver automatically becomes the default. `csilk_perm_set_default()` switches at runtime.

### 3.3 Authorization API

| Function | Role |
|---|---|
| `csilk_perm_check(ctx, perm, resource)` | Delegate to default driver → 0 if allowed |
| `csilk_perm_require(ctx, perm, resource)` | Check → abort with 403 JSON if denied |
| `csilk_perm_auto_middleware(ctx)` | Read route metadata → enforce via require |

### 3.4 Built-in "simple" RBAC Driver

**File**: `src/drivers/perm/simple.c`

The "simple" driver implements in-memory role-based access control with a fixed-size rule table (max 128 rules).

```
Rule table:
┌──────────┬──────────────┬──────────────┐
│  role     │  permission   │  resource     │
├──────────┼──────────────┼──────────────┤
│ "admin"  │ "write"      │ "*"          │
│ "editor" │ "read"       │ "articles:*" │
│ "viewer" │ "read"       │ "public:*"   │
└──────────┴──────────────┴──────────────┘
```

**Pattern matching** (`match_pattern()`):

| Pattern | Matches |
|---|---|
| `"*"` | Everything (global wildcard) |
| `"namespace:*"` | Any string starting with `"namespace:"` |
| `"exact"` | Only the exact string |

**Role resolution** (in order):

1. `csilk_get(ctx, "role")` — directly-set role key
2. `csilk_get(ctx, "jwt_payload")` → `payload.role` — from JWT token payload

**Functions**:

| Function | Role |
|---|---|
| `csilk_perm_simple_init()` | Register "simple" driver, clear rule table |
| `csilk_perm_simple_allow(role, perm, resource)` | Add a rule |
| `csilk_perm_simple_clear()` | Remove all rules |

**Check algorithm**:

```
simple_check(ctx, permission, resource):
  1. Resolve role from context (get_role_from_ctx)
  2. If no role → deny (-1)
  3. For each rule in table [0..rule_count):
     a. match_pattern(rule.role, role)
     b. match_pattern(rule.permission, permission)
     c. match_pattern(rule.resource, resource)
     d. If all three match → allow (0)
  4. No match → deny (-1)
```

---

## 4. Authentication: JWT Middleware

**File**: `src/middleware/jwt.c`

### Algorithm

```
csilk_jwt_middleware(ctx, secret):
  1. Read Authorization header
  2. If not "Bearer <token>" → 401, abort
  3. Decode + verify token (HS256):
     a. Base64url-decode header, payload, signature
     b. Verify HMAC-SHA256(header + "." + payload, secret) == signature
     c. Parse payload as cJSON
  4. Check exp claim → if expired, 401, abort
  5. Store payload as ctx["jwt_payload"] (cJSON*)
  6. csilk_next(ctx)  // pass to next handler
```

### Key Features

| Feature | Details |
|---|---|
| **Algorithm** | HS256 (HMAC-SHA256) |
| **Token source** | `Authorization: Bearer <token>` header |
| **Expiry** | Checks `exp` claim against `time()` |
| **Payload** | Stored on context as `"jwt_payload"` (cJSON pointer) |
| **Constant-time comparison** | Signature comparison uses `constant_time_compare()` to prevent timing attacks |
| **Cleanup** | `csilk_ctx_cleanup_jwt_payload()` frees the payload after handler completes |
| **Extended API** | `csilk_jwt_middleware_ex()` accepts key length + algorithm parameter for future algorithm support |

---

## 5. CSRF Protection

**File**: `src/middleware/csrf.c`

### Pattern: Double-Submit Cookie

```
Safe methods (GET, HEAD, OPTIONS):
  └─ Check if csrf_token cookie exists
     ├─ No  → Generate 16 random bytes → Set cookie (HttpOnly, path=/)
     └─ Yes → Pass through
  └─ csilk_next()

State-changing methods (POST, PUT, DELETE, etc.):
  ├─ Read X-CSRF-Token header
  │   └─ Missing → 403, abort, increment csrf_violations metric
  ├─ Read csrf_token cookie
  ├─ Compare header == cookie (strcmp)
  │   ├─ Match → csilk_next()
  │   └─ Mismatch → 403, abort, increment csrf_violations metric
```

### Token Generation

1. Primary: 16 bytes from `/dev/urandom`, formatted as 32-char hex string
2. Fallback (if `/dev/urandom` unavailable): `rand_r()` seeded with `time() ^ getpid()`

---

## 6. Web Application Firewall (WAF)

**File**: `src/middleware/waf.c`

### Attack Detection

The WAF middleware scans request path, query parameters, and form fields for known attack patterns:

| Category | Patterns (case-insensitive) |
|---|---|
| **SQL Injection** | `UNION SELECT`, `SELECT FROM`, `INSERT INTO`, `UPDATE SET`, `DELETE FROM`, `DROP TABLE`, `OR '1'='1`, `OR "1"="1`, `WAITFOR DELAY`, `SLEEP(`, `PG_SLEEP(` |
| **XSS** | `<SCRIPT`, `ONERROR=`, `ONLOAD=`, `JAVASCRIPT:`, `ALERT(` |
| **Directory Traversal** | `../`, `..\\` |

### Processing Flow

```
csilk_waf_middleware(ctx):
  1. Check request path for directory traversal patterns
  2. If not blocked: iterate query params via csilk_for_each_query()
     └─ For each value: check SQLi, XSS, traversal patterns
  3. If not blocked: iterate form fields via csilk_for_each_form_field()
     └─ For each value: check SQLi, XSS, traversal patterns
  4. If blocked: 403 JSON error + csilk_abort()
  5. If clean: csilk_next()
```

The callback (`check_pattern_cb`) stops iteration on first match, recording the matched key, value, pattern, and attack type for logging.

---

## 7. Additional Security Middleware

### 7.1 CORS Middleware

**File**: `src/middleware/cors.c`

Configurable CORS headers via `csilk_cors_config_t`:

| Parameter | Default | Header |
|---|---|---|
| `allow_origin` | `"*"` | `Access-Control-Allow-Origin` |
| `allow_methods` | `"GET, POST, PUT, DELETE, OPTIONS"` | `Access-Control-Allow-Methods` |
| `allow_headers` | `"Content-Type, Authorization"` | `Access-Control-Allow-Headers` |
| `allow_credentials` | 0 | `Access-Control-Allow-Credentials` |
| `max_age` | 86400 | `Access-Control-Max-Age` |

Handles preflight `OPTIONS` requests automatically.

### 7.2 Rate Limiting Middleware

**File**: `src/middleware/rate_limit.c`

Token-bucket rate limiter per client IP. Configurable max requests per second. When the limit is exceeded, returns 429 Too Many Requests.

### 7.3 Request ID Middleware

**File**: `src/middleware/request_id.c`

Injects/reads `X-Request-Id` header for end-to-end tracing. Generates a UUID if the client does not provide one.

---

## 8. Identity Flow (End-to-End)

```
Request → CORS → Rate Limit → JWT Auth → WAF → CSRF → Permission Check → Handler
                               │                                │
                               │ Stores jwt_payload              │
                               │ with {sub, role, exp}           │
                               ▼                                ▼
                          Context["jwt_payload"]          perm_auto_middleware
                          Context["role"] (from JWT)      reads route metadata
                                                          calls driver->check()
```

Typical integration:

```c
// Register middleware (order matters)
csilk_server_use(server, csilk_cors_middleware);       // 1. CORS
csilk_server_use(server, csilk_jwt_middleware, secret); // 2. Auth
csilk_server_use(server, csilk_waf_middleware);          // 3. WAF
csilk_server_use(server, csilk_csrf_middleware);         // 4. CSRF
csilk_server_use(server, csilk_perm_auto_middleware);    // 5. Permission

// Route with permission metadata
csilk_app_add_route_extended_perm(app, "POST", "/orders",
    handlers, 1, nullptr, nullptr, "create", "orders");
```

---

## 9. Concurrency

- **Permission registry**: Single global registry. Only written during startup (registration). Reads are lock-free (no concurrent modification after init).
- **Simple driver**: Rule table is read-only during checks (rules are only added at startup). No locking needed.
- **JWT**: Stateless. The JWT middleware allocates and frees cJSON payloads per-request. No shared state.
- **WAF/CSRF/CORS**: Per-request inspection. No shared state.
- **Rate limiter**: Thread-safe via per-IP token bucket with atomic operations.

---

## 10. Related Files

| File | Role |
|---|---|
| `src/security/perm.c` | Permission subsystem: driver registry, check, require, auto-middleware |
| `include/csilk/drivers/perm.h` | Public API: perm_driver_t, registration, check/require |
| `src/drivers/perm/simple.c` | Built-in in-memory RBAC driver with wildcard pattern matching |
| `src/middleware/jwt.c` | JWT HS256 authentication middleware + token generation |
| `src/middleware/waf.c` | WAF middleware: SQLi/XSS/traversal pattern detection |
| `src/middleware/csrf.c` | CSRF double-submit cookie protection |
| `src/middleware/cors.c` | CORS header middleware |
| `src/middleware/rate_limit.c` | Token-bucket rate limiter |
| `src/middleware/request_id.c` | X-Request-Id tracing middleware |
| `tests/test_perm.c` | Permission system tests (14 test cases, all passing) |

---

## 11. Design Rationale

**Why a single-function driver vtable instead of a full middleware chain?**  
Permission logic is fundamentally a binary decision (allow/deny) based on (identity, action, resource). A single `check()` callback covers all access control models (RBAC, ABAC, ReBAC, ACL) without constraining the policy engine. More complex workflows can be built by composing multiple permission drivers or middleware layers.

**Why JWT for identity instead of session-based auth?**  
JWT is stateless — no server-side session storage, no DB lookups on every request. The decoded payload carries the user's role and claims directly, enabling downstream middleware (especially the permission checker) to operate without additional I/O. The stateless design aligns with csilk's zero-copy, low-latency architecture.

**Why a simple rule table instead of Casbin/OPA by default?**  
The built-in driver handles the common case (role-based access with wildcard resources) in ~50 lines of matching logic. For complex policy requirements (ABAC, multi-tenant, RBAC with role hierarchies), a custom driver wrapping Casbin, OPA, or a database-backed policy store can be registered in its place.
