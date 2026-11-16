# 安全审计 — OWASP Top 10 回归 (v1.2.0)

## 审计范围

`src/`, `include/` 全模块，重点关注：
- 内存安全（buffer overflow, UAF, double-free）
- 注入（SQL, 命令, 路径）
- 认证/授权（JWT, session, CSRF）
- 传输安全（TLS 1.3, QUIC）

## 发现

### P0（已修复）

| # | 位置 | 问题 | 状态 |
|---|---|---|---|
| 1 | `src/core/http/http1_parse.c` | HTTP 请求行解析未限制 URL 长度 | ✅ 已限制 `CSILK_MAX_URL_LEN` |
| 2 | `src/middleware/waf.c` | WAF 规则匹配使用正则（ReDoS 风险） | ✅ 改为固定长度模式匹配 |
| 3 | `src/core/config/yaml.c` | YAML 解析器无深度限制（栈溢出） | ✅ 限制 64 层嵌套 |

### P1（需关注）

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| 4 | `src/drivers/db/db.c:97` | `sprintf` 写入 `$N` 参数占位符 | 已验证 buffer 预分配足够（`param_count * 12`） |
| 5 | `src/drivers/db/mysql.c` | `strtok` 非线程安全 | 改为 `strtok_r` 或使用本地 buffer |
| 6 | `src/drivers/db/redis.c:70` | `strtok` 非线程安全 | 同上 |
| 7 | `src/core/ctx/ctx.c` | `csilk_set_ex` destructor 在 arena reset 时 LIFO 调用 | 确认 destructor 无死锁风险 |

### P2（低风险）

| # | 位置 | 问题 | 说明 |
|---|---|---|---|
| 8 | `src/protocols/grpc.c` | gRPC 帧解析未验证 payload 类型 | 由上层 handler 负责验证 |
| 9 | `src/middleware/jwt.c` | JWT 签名验证使用 OpenSSL EVP | 确认 HMAC 长度检查 |
| 10 | `src/core/server/connection_pool.c` | 连接池 32 个预分配，超出后 malloc | OOM 时返回 -1，无崩溃风险 |

## OWASP Top 10 映射

| OWASP | csilk 对应模块 | 状态 |
|---|---|---|
| A01: Broken Access Control | JWT, session, RBAC | ✅ 已实现 |
| A02: Cryptographic Failures | TLS 1.3, bcrypt, AES-GCM | ✅ 已实现 |
| A03: Injection | SQL param binding, WAF | ✅ 已实现 |
| A04: Insecure Design | Arena model, lock-free pool | ✅ 架构级防护 |
| A05: Security Misconfiguration | 默认安全配置, TLS 1.3 强制 | ✅ 已实现 |
| A06: Vulnerable Components | FetchContent pinned versions | ✅ 版本锁定 |
| A07: Auth Failures | JWT middleware, rate limit | ✅ 已实现 |
| A08: Data Integrity | WAL, raft consensus, checksums | ✅ 已实现 |
| A09: Logging Failures | OTLP trace, structured logging | ✅ 已实现 |
| A10: SSRF | URL 验证, 网络隔离 | ⚠️ 需 review |

## 建议行动

1. **P1-5/6**：将 `strtok` 改为 `strtok_r`（低优先级，非线程安全但当前单线程使用）
2. **P2-10**：SSRF 防护 — 在 `csilk_url_validate` 中增加内网 IP 黑名单
3. 定期运行 ASAN/TSAN CI 矩阵（已有）
