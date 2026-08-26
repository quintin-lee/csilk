# 安全中间件使用指南

> **Version**: 0.5.2 | **Last updated**: 2026-08-24

csilk 提供了一套层次化的安全中间件体系，覆盖认证（JWT）、授权（RBAC Perm）、请求验证（CSRF、WAF）和传输层保护（CORS、Rate Limiting）。所有安全中间件 **MUST** 按以下顺序注册：

**CORS → Rate Limiter → WAF → JWT → CSRF → Permission**

推荐使用 `csilk_app_t` 高层 API 简化注册流程。

---

## 1. CORS（跨域资源共享）

### 1.1 配置结构

```c
typedef struct {
    const char* allow_origin;      // 允许的源，如 "*" 或 "https://example.com"
    const char* allow_methods;     // 允许的 HTTP 方法，如 "GET,POST,PUT,DELETE"
    const char* allow_headers;     // 允许的请求头，如 "Content-Type,Authorization"
    int         allow_credentials; // 是否允许携带凭据（cookies）
    int         max_age;           // 预检请求缓存时间（秒）
} csilk_cors_config_t;
```

### 1.2 基本用法

```c
#include "csilk/csilk.h"

void setup_cors(csilk_app_t* app) {
    csilk_cors_config_t cors = {
        .allow_origin  = "*",
        .allow_methods = "GET,POST,PUT,DELETE,OPTIONS",
        .allow_headers = "Content-Type,Authorization",
        .allow_credentials = 0,
        .max_age       = 86400,
    };

    // 注册 CORS 中间件（全局）
    csilk_app_use(app, (csilk_handler_t)csilk_cors_middleware, &cors);
}
```

> **注意**：CORS 中间件会自动处理 OPTIONS 预检请求，返回 204 No Content 并中止链。对于非 OPTIONS 请求，中间件添加 CORS 响应头后继续执行。

### 1.3 YAML 配置

```yaml
cors:
  allow_origin: "*"
  allow_methods: "GET,POST,PUT,DELETE,OPTIONS"
  allow_headers: "Content-Type,Authorization"
```

然后通过 `csilk_app_apply_config(app)` 自动应用。

---

## 2. Rate Limiting（限流）

### 2.1 基本用法

基于客户端 IP 的固定窗口计数器。超限时返回 429 Too Many Requests。

```c
void setup_rate_limit(csilk_app_t* app) {
    // 限制每个 IP 每分钟最多 100 个请求
    csilk_app_use(app, (csilk_handler_t)csilk_rate_limit_middleware, (void*)(intptr_t)100);
}
```

### 2.2 YAML 配置

```yaml
rate_limit:
  requests_per_second: 100
  burst: 50
```

> **注意**：Rate Limiter **MUST** 使用滑动窗口计数器，采用原子自增操作。当请求被限流时，中间件**SHOULD** 设置 `Retry-After` 响应头。

---

## 3. WAF（Web 应用防火墙）

### 3.1 基本用法

检查请求路径、查询参数和请求头中常见的攻击模式（SQL 注入、XSS、路径穿越等）。检测到攻击时返回 403 Forbidden。

```c
void setup_waf(csilk_app_t* app) {
    csilk_app_use(app, csilk_waf_middleware);
}
```

### 3.2 WAF Detection Scope

| Attack Type | Target Fields | Example |
|:------------|:--------------|:--------|
| SQL Injection | Path, Query Params, Form, JSON/Text Request Body | `?id=1' OR '1'='1`, `{"query": "1 UNION SELECT"}` |
| XSS | Path, Query Params, Form, JSON/Text Request Body | `<script>alert(1)</script>` |
| Path Traversal | Path, Request Body | `../../etc/passwd`, `..\..\windows\win.ini` |
| Command Injection | Query Params, Request Body | `; rm -rf /` |

---

## 4. JWT Authentication

### 4.1 Supported Algorithms & Cross-Validation

| Algorithm | Enum | Type | Key | Security Policy |
|:----------|:----:|:----:|:---:|:----------------|
| HS256 | `CSILK_JWT_HS256` | Symmetric | Secret string | Enforces Header `alg="HS256"` |
| RS256 | `CSILK_JWT_RS256` | Asymmetric | PEM RSA Private Key | Enforces Header `alg="RS256"` |
| ES256 | `CSILK_JWT_ES256` | Asymmetric | PEM ECDSA P-256 Key | Enforces Header `alg="ES256"` |

> **Security Note**: csilk decodes the JWT header and validates that the `alg` field strictly matches the server's expected algorithm, preventing `alg: "none"` bypasses and RSA/HMAC algorithm confusion attacks (CWE-327). Signature comparison uses OpenSSL `CRYPTO_memcmp()` for constant-time evaluation.

### 4.2 Basic Usage

```c
void setup_jwt(csilk_app_t* app) {
    // 注册 JWT 中间件，所有受保护路由都需要 Bearer token
    csilk_app_use(app, (csilk_handler_t)csilk_jwt_middleware, "your-secret-key");
}

// 受保护的路由处理器
void protected_handler(csilk_ctx_t* c) {
    // JWT 中间件已将解码后的 payload 存入上下文
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    if (payload) {
        const char* sub = cJSON_GetObjectItem(payload, "sub")->valuestring;
        const char* role = cJSON_GetObjectItem(payload, "role")->valuestring;
        CSILK_LOG_I("Authenticated user: %s (role: %s)", sub, role);
    }
    csilk_string(c, 200, "Access granted");
}
```

### 4.3 分组保护

```c
void setup_routes(csilk_app_t* app) {
    // 公开路由 — 无需认证
    csilk_app_get(app, "/health", csilk_health_check_handler);

    // API 路由 — 需要 JWT 认证
    csilk_app_use_group(app, "/api", (csilk_handler_t)csilk_jwt_middleware, "your-secret-key");
    csilk_app_get(app, "/api/users", list_users);
    csilk_app_post(app, "/api/users", create_user);
}
```

### 4.4 自定义认证中间件

```c
// 自定义 token 验证器
int custom_validator(const char* token) {
    // 验证 token（例如查数据库或调用外部 API）
    return (token && strcmp(token, "valid-token") == 0) ? 1 : 0;
}

void setup_custom_auth(csilk_app_t* app) {
    csilk_app_use(app, (csilk_handler_t)csilk_auth_middleware, custom_validator);
}
```

---

## 5. CSRF Protection

### 5.1 Basic Usage

Validates CSRF tokens on state-changing methods (POST, PUT, DELETE, PATCH). Rejects invalid requests with 403 Forbidden.

```c
void setup_csrf(csilk_app_t* app) {
    csilk_app_use(app, csilk_csrf_middleware);
}
```

> **How It Works**: The middleware uses a Double Submit Cookie pattern, extracting the token from the `X-CSRF-Token` header (or form field) and comparing it against the cookie value using OpenSSL `CRYPTO_memcmp()` for constant-time security (CWE-208). The CSRF cookie is set with `Secure` and `http_only = 0` so that Single Page Applications (SPAs / XHR) can read it and attach it to subsequent mutation requests. Token generation uses `/dev/urandom`, failing securely if unavailable (CWE-330).

---

## 6. RBAC 权限控制（Perm Driver）

### 6.1 初始化

```c
#include "csilk/drivers/perm.h"

void setup_perm(void) {
    // 初始化权限子系统（线程安全，可多次调用）
    csilk_perm_init();

    // 初始化内置的 in-memory RBAC 驱动（注册为 "simple"）
    csilk_perm_simple_init();

    // 设置为默认驱动
    csilk_perm_set_default("simple");
}
```

### 6.2 定义角色与权限

```c
void define_permissions(void) {
    // 管理员拥有所有权限
    csilk_perm_simple_allow("admin",   "read",   "articles:*");
    csilk_perm_simple_allow("admin",   "write",  "articles:*");
    csilk_perm_simple_allow("admin",   "delete", "articles:*");

    // 编辑者可以读写文章
    csilk_perm_simple_allow("editor",  "read",   "articles:*");
    csilk_perm_simple_allow("editor",  "write",  "articles:*");

    // 读者只能阅读
    csilk_perm_simple_allow("reader",  "read",   "articles:*");
}
```

### 6.3 路由级权限控制

#### 方案 A：自动中间件（推荐）

```c
void setup_routes_with_perm(csilk_app_t* app) {
    // 注册自动权限检查中间件（全局）
    csilk_app_use(app, csilk_perm_auto_middleware);

    // 注册路由时声明所需权限 — 中间件自动检查
    csilk_app_get_perm(app, "/articles",    list_articles,  "read",   "articles:*");
    csilk_app_post_perm(app, "/articles",   create_article, "write",  "articles:*");
    csilk_app_delete_perm(app, "/articles/:id", delete_article, "delete", "articles:{{id}}");
}
```

#### 方案 B：手动检查

```c
void admin_panel_handler(csilk_ctx_t* c) {
    // 手动执行权限检查
    if (csilk_perm_check(c, "admin", "dashboard:*") != 1) {
        csilk_json_error(c, 403, "Forbidden");
        return;
    }
    csilk_string(c, 200, "Admin panel");
}

// 或者使用便捷封装
void delete_user_handler(csilk_ctx_t* c) {
    csilk_perm_require(c, "delete", "users:*");
    // 如果未通过，上面的调用已经 abort 并返回 403
    csilk_string(c, 200, "User deleted");
}
```

### 6.4 自定义权限驱动

```c
// 实现自定义驱动接口
int my_check(csilk_ctx_t* c, const char* permission, const char* resource) {
    // 从 context 获取用户身份
    cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
    const char* role = cJSON_GetObjectItem(payload, "role")->valuestring;

    // 实现策略逻辑（例如调用 Casbin、OPA 等外部引擎）
    if (strcmp(role, "superadmin") == 0) return 1;
    return 0;
}

static csilk_perm_driver_t my_driver = {
    .name  = "my_custom",
    .check = my_check,
};

void setup_custom_perm(void) {
    csilk_perm_init();
    csilk_perm_register_driver("my_custom", &my_driver);
    csilk_perm_set_default("my_custom");
}
```

---

## 7. 完整安全配置示例

以下示例展示了如何组合所有安全中间件：

```c
#include "csilk/csilk.h"
#include "csilk/drivers/perm.h"

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");

    // === 1. CORS（最外层）=== 
    csilk_cors_config_t cors = {
        .allow_origin  = "https://myapp.example.com",
        .allow_methods = "GET,POST,PUT,DELETE",
        .allow_headers = "Content-Type,Authorization",
    };
    csilk_app_use(app, (csilk_handler_t)csilk_cors_middleware, &cors);

    // === 2. Rate Limiter ===
    csilk_app_use(app, (csilk_handler_t)csilk_rate_limit_middleware, (void*)100);

    // === 3. WAF ===
    csilk_app_use(app, csilk_waf_middleware);

    // === 4. Recovery（安全兜底） ===
    csilk_app_use(app, csilk_recovery_handler);

    // === 5. RBAC 初始化 ===
    csilk_perm_init();
    csilk_perm_simple_init();
    csilk_perm_set_default("simple");

    csilk_perm_simple_allow("admin",  "read",   "*:*");
    csilk_perm_simple_allow("admin",  "write",  "*:*");
    csilk_perm_simple_allow("editor", "read",   "articles:*");
    csilk_perm_simple_allow("editor", "write",  "articles:*");

    // === 6. 自动权限检查中间件 ===
    csilk_app_use(app, csilk_perm_auto_middleware);

    // === 7. 公开路由 ===
    csilk_app_get(app, "/health", csilk_health_check_handler);
    csilk_app_get(app, "/login", login_handler);

    // === 8. API 路由（JWT 保护） ===
    csilk_app_use_group(app, "/api",
        (csilk_handler_t)csilk_jwt_middleware, "my-secret-key-12345");
    csilk_app_use_group(app, "/api", csilk_csrf_middleware);

    csilk_app_get_perm(app,  "/api/articles",    list_articles,  "read",  "articles:*");
    csilk_app_post_perm(app, "/api/articles",    create_article, "write", "articles:*");
    csilk_app_delete_perm(app, "/api/articles/:id", delete_article, "delete", "articles:{{id}}");

    // === 9. 启动 ===
    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

### 执行顺序

```
请求进入
  │
  ├─ CORS（添加跨域头，处理 OPTIONS 预检）
  ├─ Rate Limiter（检查请求频率）
  ├─ WAF（扫描恶意负载）
  ├─ Recovery（捕获异常）
  ├─ Perm Auto（匹配路由权限元数据）
  │
  ├─ [/api 组]
  │   ├─ JWT（验证 Bearer token）
  │   ├─ CSRF（校验 token）
  │   └─ 路由处理器
  │
  └─ [/health]
      └─ 路由处理器
```

---

## 8. 安全最佳实践

| 实践 | 说明 |
|:-----|:------|
| **MUST** 使用 TLS 1.3 | 生产环境必须启用 HTTPS，设置 `tls_min_version = TLS_VERSION_1_3` |
| **MUST** JWT 密钥使用强随机数 | 使用 `csilk_crypto_fill_random()` 生成密钥，长度 ≥ 32 字节 |
| **SHOULD** 限流开启 | 至少设置每 IP 每分钟 100 请求的保护 |
| **SHOULD** WAF 全局启用 | 路径穿越和 XSS 防护开销极低 (O(n) DFA) |
| **SHOULD** RBAC 与 JWT 配合 | JWT paylaod 中的 `role` 字段供 Perm driver 做 RBAC 决策 |
| **MAY** 自定义 Perm Driver | 集成 Casbin、OPA 等外部策略引擎 |
| **MUST NOT** 在生产关闭 Recovery | Recovery 中间件防止崩溃导致整个 worker 退出 |

---

---

## 9. Security Headers & CRLF Injection Prevention

### 9.1 Security Headers Middleware

csilk provides `csilk_security_headers_middleware()` to automatically append defensive HTTP headers:

```c
#include "csilk/core/security_headers.h"

void setup_security_headers(csilk_app_t* app) {
    csilk_app_use(app, csilk_security_headers_middleware);
}
```

| Header | Value | Purpose |
|:-------|:------|:--------|
| `X-Frame-Options` | `DENY` | Prevents Clickjacking |
| `X-Content-Type-Options` | `nosniff` | Disables MIME type sniffing |
| `X-XSS-Protection` | `0` | Disables legacy buggy XSS auditors |
| `Referrer-Policy` | `strict-origin-when-cross-origin` | Limits Referrer exposure |

### 9.2 CRLF Response Splitting Protection (CWE-113)

When response headers are stored in `map_set` and `map_add`, `\r` and `\n` control characters are automatically stripped from both header keys and values, immunizing against HTTP response splitting and header injection attacks.

---

## 10. Cookie Security & SameSite Attribute

csilk provides fine-grained cookie control via `csilk_set_cookie_ex()`, fully supporting the `SameSite` attribute:

```c
// Set a secure cookie with SameSite policy
csilk_set_cookie_ex(c, "session_id", sid, 86400, "/", "example.com", 1, 1, "Strict");
```

| Attribute | Description | Recommended Setting |
|:----------|:------------|:--------------------|
| `Secure` | Enforces HTTPS-only transmission | `1` |
| `HttpOnly` | Restricts access from JavaScript | `1` (Session cookies) / `0` (CSRF cookies) |
| `SameSite` | Cross-site request policy (`Strict` / `Lax` / `None`) | `Lax` or `Strict` |

---

## 11. Multipart Upload & Binary Safety

The multipart parser uses length-bounded binary searching (`_csilk_memmem`), safely handling binary uploads containing `\0` bytes (images, PDFs, archives) without boundary truncation, backed by built-in limits:

| Limit Item | Value | Description |
|:-----------|:------|:------------|
| `CSILK_MAX_PART_NAME` | 128 bytes | Max part field name length |
| `CSILK_MAX_PART_FILENAME` | 256 bytes | Max uploaded file name length |
| `CSILK_MAX_PART_SIZE` | 10 MB | Max single part size |

---

## 12. Database Security & Parameterized DML Execution (CWE-89)

To prevent SQL injection in data modification operations, csilk provides `csilk_db_exec_param()` across all database drivers:

```c
// Safe parameterized DML: escapes and binds parameters automatically
const char* params[] = { "john_doe", "john@example.com" };
int affected = csilk_db_exec_param(pool, 
    "INSERT INTO users (username, email) VALUES (?, ?)", 
    params, 2);
```

---

## Further Reading

| Document | Content |
|:---------|:--------|
| [Module Design — Security](../../docs/module-design/security.md) | Internal security architecture and driver design |
| [Configuration Guide](./configuration.md) | Full YAML configuration reference |
