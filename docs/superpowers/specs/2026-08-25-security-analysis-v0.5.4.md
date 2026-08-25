# csilk Security Analysis Report v0.5.4

- **版本**: v0.5.3 → v0.5.4
- **日期**: 2026-08-25
- **审计范围**: 全部源代码 (195 C 源文件, 53 头文件)
- **审计方法**: 静态代码分析 + 手动代码审查 + OWASP Top 10 覆盖
- **目标**: 在 v0.5.3 修复基础上，发现并修复剩余安全问题

---

## 执行摘要

| 状态 | 数量 | 说明 |
|------|------|------|
| ✅ 已修复 (v0.5.3) | 7 | SQL注入、SSRF、bcrypt、CORS、速率限制、Gzip、mkstemp |
| 🔴 待修复 | 2 | CSRF弱熵回退、Session cookie缺Secure (Critical) |
| 🟠 待修复 | 3 | CSRF cookie缺Secure、XSS防护头缺失、multipart无大小限制 (High) |
| 🟡 待修复 | 3 | OTLP rand()、xdp_waf atoi、config timeout atoi (Medium) |

**总计待修复**: 8 个问题 (2 Critical + 3 High + 3 Medium)

---

## 1. Critical 级别问题

### 1.1 CSRF Token 降级到弱随机数 (CWE-330)

**文件**: `src/middleware/csrf.c:122-133`

```c
// 当前: /dev/urandom 不可用时降级到 rand_r()
if (!fp) {
    CSILK_LOG_W("CSRF: Failed to open /dev/urandom. Falling back to ...");
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    snprintf(buf, buf_size, "%08x%08x%08x%08x",
             rand_r(&seed), rand_r(&seed), rand_r(&seed), rand_r(&seed));
}
```

**风险**: 攻击者可以预测 token（已知时间+PID），实现 CSRF 攻击。
**修复**: 移除降级路径，/dev/urandom 不可用时直接返回错误。

### 1.2 Session Cookie 缺少 Secure 标志 (CWE-1004)

**文件**: `src/middleware/session.c:309`

```c
// 当前: secure=0 (不强制 HTTPS)
csilk_set_cookie(c, SESSION_COOKIE, session->id, 60 * 60 * 24, "/", NULL, 0, 1);
```

**风险**: Session cookie 可通过明文 HTTP 传输，易被中间人窃取（会话劫持）。
**修复**: 设置 secure=1，强制仅通过 HTTPS 传输。

---

## 2. High 级别问题

### 2.1 CSRF Token Cookie 缺少 Secure 标志 (CWE-1004)

**文件**: `src/middleware/csrf.c:54`

```c
// 当前: secure=0
csilk_set_cookie(c, "csrf_token", token_buf, 86400, "/", NULL, 0, 1);
```

**修复**: 设置 secure=1。

### 2.2 安全响应头缺失 (CWE-79 XSS, CWE-1021 Clickjacking)

**缺失的头**:
- `X-Frame-Options: DENY` (防点击劫持)
- `X-Content-Type-Options: nosniff` (防 MIME 嗅探)
- `X-XSS-Protection: 0` (现代浏览器已弃用，但兼容旧版)
- `Content-Security-Policy` (可选，由应用层定义)
- `Strict-Transport-Security` (HSTS，可选)

**修复**: 在 `src/middleware/logger.c` 或新增 `security_headers.c` 中添加默认安全头中间件。

### 2.3 Multipart 上传无文件大小限制 (CWE-434)

**文件**: `src/middleware/multipart.c`

- 有 `CSILK_MAX_PART_NAME=128` 和 `CSILK_MAX_PART_FILENAME=256`
- **无** 对 part body 大小的限制
- 仅依赖全局 `max_body_size`，但 multipart 解析过程中内存分配不受控

**风险**: 大文件上传可导致 DoS 或内存耗尽。
**修复**: 添加 `CSILK_MAX_PART_SIZE` 常量，在解析时检查每个 part 的大小。

---

## 3. Medium 级别问题

### 3.1 OTLP Trace Span ID 使用 rand() (CWE-330)

**文件**: `src/middleware/otlp_trace.c:174`

```c
snprintf(span->span_id, sizeof(span->span_id), "%016lx", (unsigned long)rand());
```

**风险**: span ID 可预测，影响分布式追踪的唯一性保证。
**修复**: 使用 OpenSSL RAND_bytes 或 /dev/urandom 替换 rand()。

### 3.2 xdp_waf atoi 无范围验证 (CWE-284)

**文件**: `src/middleware/xdp_waf.c:83`

```c
prefix_len = (uint32_t)atoi(slash + 1);
```

**风险**: 负数或超大值可能导致无效 CIDR 配置。
**修复**: 添加范围检查 (0-128)。

### 3.3 Config Timeout 配置 atoi 无验证 (CWE-284)

**文件**: `src/core/config/config.c:146-151`

```c
config->server.idle_timeout_ms = atoi(val);
config->server.read_timeout_ms = atoi(val);
// ... 等多个 timeout 字段
```

**风险**: 负数或极大超时值可导致 DoS。
**修复**: 添加范围验证 (0-3600000ms)。

---

## 4. OWASP Top 10 映射 (v0.5.4)

| OWASP 2021 | csilk 状态 | 关键 CWE |
|------------|-----------|----------|
| A01: 访问控制失效 | ✅ 已修复 | CWE-284, CWE-862 |
| A02: 加密失败 | ⚠️ 部分修复 | CWE-330 (CSRF fallback) |
| A03: 注入 | ✅ 已修复 | CWE-89 |
| A04: 不安全设计 | ⚠️ 部分修复 | CWE-1004 (cookies) |
| A05: 安全配置错误 | ⚠️ 部分修复 | CWE-284 (atoi) |
| A06: 脆弱组件 | ℹ️ 第三方依赖 | CWE-1104 |
| A07: 认证与识别失败 | ✅ 已修复 | CWE-287, CWE-306 |
| A08: 软件与数据完整性 | ⚠️ 文件上传待修复 | CWE-434 |
| A09: 安全日志与监控 | ⚠️ 缺安全头 | CWE-778 |
| A10: SSRF | ✅ 已修复 | CWE-918 |

---

## 5. 修复优先级

### P0 (立即修复)
1. **CSRF 弱熵回退** — 移除 rand_r() 降级路径
2. **Session Cookie Secure** — 设置 secure=1

### P1 (高优先级)
3. **CSRF Cookie Secure** — 设置 secure=1
4. **安全响应头** — 添加 X-Frame-Options, X-Content-Type-Options
5. **Multipart 大小限制** — 添加 CSILK_MAX_PART_SIZE

### P2 (中优先级)
6. **OTLP rand()** — 替换为安全随机数
7. **xdp_waf atoi** — 添加范围验证
8. **Config timeout atoi** — 添加范围验证

---

## 6. 测试建议

- [ ] 添加 CSRF token 熵源测试
- [ ] 添加 Session cookie Secure 标志测试
- [ ] 添加安全响应头测试
- [ ] 添加 multipart 大小限制测试
- [ ] 定期运行 OWASP ZAP 扫描

---

**审计完成日期**: 2026-08-25
**审计工具**: 静态分析 + 手动代码审查 + OWASP Top 10 覆盖
**下次审计建议**: 每季度或重大版本发布前
