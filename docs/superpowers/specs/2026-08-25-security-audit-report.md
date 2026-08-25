# csilk Security Audit Report v0.5.2

- **版本**: v0.5.2
- **日期**: 2026-08-25
- **审计范围**: 全部源代码 (195 C 源文件, 53 头文件)
- **审计方法**: 静态代码分析 + 自动化工具扫描 + 手动审查

---

## 执行摘要

| 严重度 | 数量 | 说明 |
|--------|------|------|
| **Critical** | 3 | 未认证管理面板、SQL 注入、JWT 算法混淆 |
| **High** | 5 | TLS 配置、开放重定向、弱 CSRF、文件上传、响应分裂 |
| **Medium** | 4 | 时序侧信道、缺少认证声明验证、头注入、速率限制 |
| **Low** | 3 | SHA-1 使用、信息泄露、单调随机数 |
| **总计** | **15** | |

---

## 1. Critical 严重漏洞

### 1.1 未认证的管理面板暴露 (CWE-306)

**文件:** `src/app/admin.c:325-327`

```c
void csilk_admin_serve(csilk_app_t* app, const char* path)
{
    csilk_admin_serve_secure(app, path, NULL);  // auth_middleware = NULL!
}
```

**问题:** 默认调用 `csilk_admin_serve_secure()` 时 `auth_middleware` 为 `NULL`，导致管理面板无需认证即可访问。

**影响:** 攻击者可访问内部统计信息、MQ 监控、工作流管理、AI 引擎状态等敏感功能。

**修复建议:**
- 默认启用基础认证或使用环境变量控制
- 强制要求提供认证中间件
- 在生产配置中禁用管理面板

---

### 1.2 SQL 注入风险 (CWE-89)

**文件:** 
- `src/drivers/db/sqlite.c:244` — `sqlite3_exec(conn->db, sql, ...)`
- `src/drivers/db/mysql.c:237` — `mysql_real_query(conn->db, sql, strlen(sql))`
- `src/drivers/db/postgres.c:141` — `PQexec(conn->db, sql)`

**问题:** 三个数据库驱动均接受原始 SQL 字符串并直接执行，未提供参数化查询接口。

**影响:** 如果应用层将用户输入拼接到 SQL 语句，可导致 SQL 注入。

**当前防护:**
- 项目文档明确要求使用参数化查询
- WAF 中间件检测常见 SQL 注入模式
- 但框架层面无法强制 enforce

**修复建议:**
- 添加带参数绑定的查询 API（如 `sqlite3_bind_text`）
- 废弃或直接移除 `csilk_db_exec_raw()` 函数
- 在文档中强调安全风险

---

### 1.3 JWT 算法混淆攻击风险 (CWE-327)

**文件:** `src/middleware/jwt.c:253`

**问题:** 当前实现通过请求中的 `alg` 头确定验证算法。攻击者可将 `alg` 改为 `HS256` 并使用公钥作为 HMAC 密钥签名，绕过 RS256/ES256 验证。

**影响:** 完全绕过 JWT 验证，获取任意用户身份。

**修复建议:**
- 在 `csilk_jwt_options_t` 中固定算法类型
- 拒绝与预期算法不匹配的令牌
- 参考 NIST SP 800-57 Part 3 建议

---

## 2. High 严重漏洞

### 2.1 TLS 配置不当 (CWE-326)

**文件:** `src/core/http/tls.c:75-119`

**问题:**
- 未显式设置 TLS 版本限制（允许 TLS 1.0/1.1）
- 未配置 cipher suite 白名单
- 缺少 OCSP stapling 支持

**影响:** 易受 POODLE、BEAST 等已知 TLS 攻击。

**修复建议:**
```c
SSL_CTX_set_min_proto_version(s->ssl_ctx, TLS1_2_VERSION);
SSL_CTX_set_max_proto_version(s->ssl_ctx, TLS1_3_VERSION);
// 配置强 cipher suite
```

---

### 2.2 开放重定向 (CWE-601)

**文件:** `src/core/primitives/response.c`

**问题:** `Location` 头直接使用用户输入，未验证是否为合法 URL。

**影响:** 可用于 phishing 攻击或 OAuth 回调劫持。

**修复建议:**
- 验证 `Location` 头不以 `http://` 或 `https://` 开头
- 或只允许相对路径

---

### 2.3 CSRF 保护弱 (CWE-352)

**文件:** `src/middleware/csrf.c:40-55`

**问题:**
- Token 生成回退路径使用 `rand_r()` + `time() ^ getpid()` 种子
- 未设置 `SameSite=Strict` cookie 属性
- 依赖客户端 JavaScript 读取 cookie

**影响:** 在弱随机数环境下可能预测 token。

**修复建议:**
- 移除 `rand_r()` 回退，强制要求 `/dev/urandom`
- 添加 `SameSite=Strict` 属性
- 使用隐藏表单字段替代 cookie 读取

---

### 2.4 无限制文件上传 (CWE-434)

**文件:** `src/middleware/multipart.c:47-260`

**问题:** 解析 multipart 表单时无文件类型验证、无扩展名过滤、无大小限制。

**影响:** 攻击者可上传恶意文件（PHP、WebShell）并执行。

**修复建议:**
- 添加 MIME 类型白名单检查
- 验证文件扩展名
- 设置最大文件大小限制

---

### 2.5 HTTP 响应分裂 (CWE-113)

**文件:** `src/core/primitives/response.c`

**问题:** 响应头值直接使用用户输入，未过滤 `\r\n` 字符。

**影响:** 攻击者可注入额外 HTTP 头或响应体。

**修复建议:**
- 在设置任何头值前过滤控制字符
- 或使用 `csilk_set_header_sanitized()` 函数

---

## 3. Medium 严重漏洞

### 3.1 时序侧信道 (CWE-208) — 已部分修复

**文件:** `src/middleware/jwt.c:282-285`

**状态:** P1-1 已修复，但需验证所有代码路径。

---

### 3.2 缺少认证声明验证 (CWE-287)

**文件:** `src/middleware/jwt.c`

**问题:** JWT 验证未强制检查 `iss` (issuer)、`aud` (audience) 声明。

**影响:** 可能接受来自不可信发行者的令牌。

**修复建议:** 添加可配置的 `iss` 和 `aud` 验证选项。

---

### 3.3 Header 注入向量 (CWE-113)

**文件:** `src/core/primitives/header_map.c`

**问题:** 头字段名/值未验证非法字符。

**影响:** 可能导致响应分裂或头覆盖。

---

### 3.4 速率限制粒度不足 (CWE-770)

**文件:** `src/middleware/ratelimit.c`

**问题:** 仅基于 IP 限制，未支持 per-user、per-endpoint 限制。

**影响:** 单用户可发起大量请求。

---

## 4. Low 严重漏洞

### 4.1 SHA-1 用于 WebSocket 握手 (CWE-328)

**文件:** `src/crypto/sha1.c`, `src/protocols/websocket.c`

**状态:** 符合 RFC 6455 规范，非安全风险。但应文档化说明此限制。

---

### 4.2 OpenSSL 错误信息泄露 (CWE-200)

**文件:** `src/core/http/tls.c:82-84`

**问题:** `ERR_print_errors_fp(stderr)` 可能暴露服务器内部路径。

**修复建议:** 生产环境使用自定义错误回调。

---

### 4.3 单调随机数 (CWE-330)

**文件:** `src/crypto/crypto.c`

**问题:** 某些非安全上下文中使用单调计数器作为随机源。

---

## 5. 安全架构评估

### 5.1 优点 ✅

| 方面 | 评价 |
|------|------|
| **模块化安全中间件** | JWT、CSRF、WAF、CORS 可独立配置 |
| **常量时间比较** | JWT 签名、bcrypt 验证使用 `CRYPTO_memcmp` |
| **参数化查询接口** | 提供安全的 DB API（需应用层正确使用） |
| **输入过滤** | WAF 检测 SQLi/XSS/路径遍历 |
| **权限系统** | 路由级 RBAC，插件化驱动 |

### 5.2 待改进 ⚠️

| 方面 | 建议 |
|------|------|
| **默认安全配置** | 管理面板应默认启用认证 |
| **输出编码** | 添加自动 XSS 防护 |
| **安全头** | 默认设置 CSP、HSTS、X-Frame-Options |
| **日志安全** | 敏感数据脱敏（密码、token） |
| **安全更新** | 建立 CVE 响应流程 |

---

## 6. OWASP Top 10 映射

| OWASP 2021 | csilk 状态 | 相关 CWE |
|------------|-----------|----------|
| A01: 访问控制失效 | ⚠️ 部分防护 | CWE-285, CWE-862 |
| A02: 加密失败 | ⚠️ TLS 配置待加强 | CWE-326, CWE-327 |
| A03: 注入 | 🔴 SQL 注入风险 | CWE-89 |
| A04: 不安全设计 | ⚠️ 认证默认关闭 | CWE-306 |
| A05: 安全配置错误 | 🔴 TLS/CSRF 配置 | CWE-284, CWE-352 |
| A06: 脆弱组件 | ℹ️ 依赖第三方库 | CWE-1104 |
| A07: 认证与识别失败 | ⚠️ JWT 算法混淆 | CWE-287, CWE-327 |
| A08: 软件与数据完整性 | ⚠️ 文件上传无验证 | CWE-434 |
| A09: 安全日志与监控 | ℹ️ 有日志但缺告警 | CWE-778 |
| A10: SSRF | ⚠️ AI 驱动无 URL 限制 | CWE-918 |

---

## 7. 修复优先级建议

### 立即修复 (P0)
1. **管理面板认证** — `src/app/admin.c`
2. **JWT 算法固定** — `src/middleware/jwt.c`
3. **SQL 注入防护** — 添加参数化查询 API

### 高优先级 (P1)
4. **TLS 配置加固** — `src/core/http/tls.c`
5. **CSRF 增强** — `src/middleware/csrf.c`
6. **文件上传验证** — `src/middleware/multipart.c`
7. **响应头注入防护** — `src/core/primitives/response.c`

### 中优先级 (P2)
8. **JWT 声明验证** — `iss`, `aud` 检查
9. **速率限制细化** — per-user/endpoint
10. **安全头默认值** — CSP, HSTS, X-Frame-Options

### 低优先级 (P3)
11. **错误信息脱敏** — 生产环境日志
12. **SHA-1 文档化** — WebSocket 限制说明

---

## 8. 测试建议

- [ ] 添加 JWT 算法混淆攻击测试
- [ ] 添加 SQL 注入 fuzzing 测试
- [ ] 添加 CSRF token 预测测试
- [ ] 添加路径遍历绕过测试
- [ ] 添加文件上传类型绕过测试
- [ ] 定期运行 OWASP ZAP 扫描

---

**审计完成日期**: 2026-08-25  
**审计工具**: 静态分析 + 手动代码审查  
**下次审计建议**: 每季度或重大版本发布前
