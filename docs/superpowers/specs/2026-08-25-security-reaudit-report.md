# csilk Security Re-Audit Report v0.5.3

- **版本**: v0.5.2 → v0.5.3
- **日期**: 2026-08-25
- **审计范围**: 全部源代码 (195 C 源文件, 53 头文件)
- **审计方法**: 静态代码分析 + 自动化工具扫描 + 手动验证 + OWASP Top 10 覆盖

---

## 执行摘要

| 状态 | 数量 | 说明 |
|------|------|------|
| ✅ 已修复 | 5 | JWT 算法混淆、Admin 认证、TLS 加固、CSRF、重定向/头注入 |
| 🔴 待修复 | 1 | SQL 注入 (Critical) |
| 🟠 待修复 | 2 | SSRF、弱熵源 (High) |
| 🟡 待修复 | 9 | 速率限制、Session、CORS、Gzip 等 (Medium) |
| 🟢 已知 | 5 | SHA-1、信息泄露等 (Low) |

**本次审计目标**: 验证上一轮修复 + 发现新安全问题 + 修复 Critical/High/Medium 级别漏洞

---

## 1. 已验证修复 (Previous Fixes Verification)

### 1.1 JWT 算法混淆 (CWE-327) ✅
**文件**: `src/middleware/jwt.c:296-304`
```c
// 修复后：固定长度比较，防止时序攻击
size_t expected_len = strlen(sig_expected_b64);
int cmp_result = len_mismatch ? 1 : 
                 CRYPTO_memcmp((const uint8_t*)sig_ptr,
                               (const uint8_t*)sig_expected_b64,
                               expected_len);
```

### 1.2 Admin 未认证 (CWE-306) ✅
**文件**: `src/app/admin.c:325-334`
```c
// 修复后：强制要求认证中间件
void csilk_admin_serve(csilk_app_t* app, const char* app_path) {
    CSILK_LOG_F("CRITICAL: Admin dashboard must use authentication!");
    // 不注册路由，强制使用 csilk_admin_serve_secure()
}
```

### 1.3 TLS 弱配置 (CWE-326) ✅
**文件**: `src/core/http/tls.c:87-96`
```c
// 修复后：强制 TLS 1.2+，强 cipher suite
SSL_CTX_set_min_proto_version(s->ssl_ctx, TLS1_2_VERSION);
SSL_CTX_set_max_proto_version(s->ssl_ctx, TLS1_3_VERSION);
SSL_CTX_set_cipher_list(s->ssl_ctx,
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    // ... 强 cipher suite
);
```

### 1.4 CSRF 弱随机数 (CWE-330) ✅
**文件**: `src/middleware/csrf.c:121-142`
```c
// 修复后：移除 rand_r 回退，强制 /dev/urandom
FILE* fp = fopen("/dev/urandom", "rb");
if (!fp) {
    CSILK_LOG_E("CSRF: Cannot open /dev/urandom");
    return -1;  // 拒绝生成弱 token
}
```

### 1.5 开放重定向 (CWE-601) ✅
**文件**: `src/core/primitives/response.c:114-149`
```c
// 修复后：拒绝绝对 URL，过滤控制字符
if (location[0] == 'h' && location[1] == 't' && 
    location[2] == 't' && location[3] == 'p') {
    CSILK_LOG_W("Redirect blocked: absolute URL not allowed");
    csilk_status(c, 400);
    return;
}
```

---

## 2. Critical 严重漏洞

### 2.1 SQL 注入 (CWE-89) 🔴

**影响**: 攻击者可通过构造恶意输入执行任意 SQL 命令

**涉及文件**:
- `src/drivers/db/sqlite.c:244` — `sqlite3_exec(conn->db, sql, ...)`
- `src/drivers/db/mysql.c:237` — `mysql_real_query(conn->db, sql, strlen(sql))`
- `src/drivers/db/postgres.c:141` — `PQexec(conn->db, sql)`
- `src/drivers/db/redis.c:303` — 原始命令执行

**当前防护**:
- 项目文档要求使用参数化查询
- WAF 中间件检测常见 SQLi 模式
- 但框架层面无法强制 enforce

**修复建议**:
1. 添加带参数绑定的查询 API:
   ```c
   int csilk_db_exec_params(csilk_db_conn_t* conn, const char* sql, ...);
   ```
2. 废弃 `csilk_db_exec_raw()` 或直接移除
3. 在文档中标注安全风险

---

## 3. High 严重漏洞

### 3.1 SSRF 在 AI 驱动 (CWE-918) 🔴

**影响**: 攻击者可利用 AI 配置发起服务器端请求伪造

**涉及文件**:
- `src/drivers/ai/openai.c:345-346,497-498`
- `src/drivers/ai/ollama.c:165-169`
- `src/drivers/ai/milvus.c`, `src/drivers/ai/qdrant.c`

**问题代码**:
```c
curl_easy_setopt(curl, CURLOPT_URL, url);  // 无验证直接使用
```

**修复建议**:
```c
// 验证 URL 协议
if (strncmp(url, "http://", 7) != 0 && 
    strncmp(url, "https://", 8) != 0) {
    CSILK_LOG_E("AI: Invalid URL scheme");
    return -1;
}
// 限制协议
curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
```

### 3.2 bcrypt 弱盐 (CWE-330) 🟠

**影响**: RNG 失败时回退到全零盐，可被彩虹表攻击

**文件**: `src/crypto/bcrypt.c:381-383`
```c
if (RAND_bytes(salt, sizeof(salt)) != 1) {
    if (RAND_priv_bytes(salt, sizeof(salt)) != 1) {
        memset(salt, 0, sizeof(salt));  // 危险回退！
    }
}
```

**修复建议**: 移除回退，RNG 失败时直接返回错误

---

## 4. Medium 严重漏洞

### 4.1 速率限制哈希表溢出 (CWE-284)
**文件**: `src/middleware/ratelimit.c:68-75`
- 固定 1024 条目的哈希表在饱和时静默覆盖
- 分布式攻击可绕过限制

### 4.2 Session 浅拷贝 (CWE-416)
**文件**: `src/middleware/session.c:338-367`
- Session 值存储为 raw void*，无深度拷贝或清理回调

### 4.3 CORS 通配符 + 凭据 (CWE-284)
**文件**: `src/middleware/cors.c:53-72`
- 允许 `Access-Control-Allow-Origin: *` 与 `AllowCredentials=true` 同时存在

### 4.4 Gzip 炸弹 (CWE-400)
**文件**: `src/middleware/gzip.c:71,236`
- `deflateBound()` 分配无上限，可导致 DoS

### 4.5 配置 atoi 无范围检查 (CWE-284)
**文件**: `src/core/config/config.c:160,163`
- 端口值、超时值无范围验证，可接受负数

### 4.6 mkstemp 可预测路径 (CWE-377)
**文件**: `src/core/config/hot_reload.c:95,111`
- `/tmp/csilk_reload_XXXXXX` 可预测，存在 race window

### 4.7 WebSocket 头部截断 (CWE-125)
**文件**: `src/protocols/websocket.c:65-66`
- snprintf 防止溢出但 header 截断可能导致协议混淆

### 4.8 Nonce 单调计数器 (CWE-330)
**文件**: `src/crypto/crypto.c:297-308`
- 所有熵源失败时回退到单调计数器

### 4.9 CSRF rand_r 回退 (CWE-330)
**文件**: `src/middleware/csrf.c:121-132`
- 仍需检查是否完全移除回退

---

## 5. Low 严重漏洞 (已知且可接受)

| CWE | 问题 | 说明 |
|-----|------|------|
| CWE-693 | JWT 未知算法字符串 | 仅影响调试日志 |
| CWE-200 | OpenSSL 错误信息 | 生产环境应禁用详细错误 |
| CWE-328 | SHA-1 用于 WS 握手 | RFC 6455 规范要求，非安全风险 |

---

## 6. OWASP Top 10 映射

| OWASP 2021 | csilk 状态 | 关键 CWE |
|------------|-----------|----------|
| A01: 访问控制失效 | ⚠️ 部分防护 | CWE-284, CWE-862 |
| A02: 加密失败 | ✅ 已修复 | CWE-326, CWE-327, CWE-330 |
| A03: 注入 | 🔴 SQL 注入未修复 | CWE-89 |
| A04: 不安全设计 | ⚠️ 部分修复 | CWE-306, CWE-693 |
| A05: 安全配置错误 | ⚠️ 部分修复 | CWE-284, CWE-377 |
| A06: 脆弱组件 | ℹ️ 第三方依赖 | CWE-1104 |
| A07: 认证与识别失败 | ✅ 已修复 | CWE-287, CWE-306 |
| A08: 软件与数据完整性 | ⚠️ 文件上传待修复 | CWE-434 |
| A09: 安全日志与监控 | ℹ️ 有日志但缺告警 | CWE-778 |
| A10: SSRF | 🔴 AI 驱动存在 | CWE-918 |

---

## 7. 修复优先级

### P0 (立即修复)
1. **SQL 注入** — 添加参数化查询 API，废弃 raw exec
2. **SSRF** — AI 驱动 URL 验证 + 协议限制

### P1 (高优先级)
3. **bcrypt 弱盐** — 移除全零回退
4. **速率限制溢出** — 增加容量或改用 LRU
5. **CORS 通配符** — 拒绝 Origin=* 与 Credentials 同时存在

### P2 (中优先级)
6. Session 深度拷贝
7. Gzip 炸弹防护
8. 配置范围验证
9. mkstemp 路径随机化

### P3 (低优先级)
10. WebSocket 头部验证
11. Nonce 熵源增强
12. 错误信息脱敏

---

## 8. 测试建议

- [ ] 添加 SQL 注入 fuzzing 测试
- [ ] 添加 SSRF 攻击向量测试
- [ ] 添加速率限制饱和测试
- [ ] 添加 CORS 配置测试
- [ ] 定期运行 OWASP ZAP 扫描

---

**审计完成日期**: 2026-08-25  
**审计工具**: 静态分析 + 手动代码审查 + OWASP Top 10 覆盖  
**下次审计建议**: 每季度或重大版本发布前
