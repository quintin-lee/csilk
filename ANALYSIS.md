# csilk 代码分析报告 — 后续优化完善点

> 生成日期: 2026-05-24 | 基于 commit `90a2d4c`
> 最后更新: 2026-05-24 | 15 项优化已完成

---

## 优先级总览
| 优先级 | 待办数 | 说明 |
|--------|:------:|------|
| **P0 — 严重** | 0 | ✅ 全部修复 |
| **P1 — 高** | 1 | HTTPS/TLS 支持 (v0.4.0 核心) |
| **P2 — 中** | 1 | Prometheus 指标支持 |
| **P3 — 低** | 1 | 性能基准测试集 |
| **合计** | **3** | |

---

## P0 — 严重缺陷

...

---

## P1 — 高优先级

### P1-5: 缺失 HTTPS/TLS 加密支持
- **状态**: [ ] 计划中 (v0.4.0)
- **方案**: 调研 OpenSSL 集成方案，利用 libuv 异步 I/O 实现安全传输。

---

## P2 — 中优先级

### P2-7: 缺少可观测性指标 (Prometheus)
- **状态**: [ ] 计划中 (v0.4.0)
- **方案**: 实现 Metrics 中间件，自动收集 QPS/响应耗时。

---

## P3 — 低优先级 & 代码质量

### P3-8: 缺乏自动化性能基准测试集
- **状态**: [ ] 计划中 (v0.4.0)
- **方案**: 集成 wrk 脚本，建立基准版本报告。

### P0-1: Session 全局链表无锁保护
- **状态**: [x] 已修复
- **位置**: `src/middleware/session.c`
- **方案**: 引入 `uv_mutex_t` 对全局 `session_store` 进行加锁保护，确保多线程下 Session 安全。

### P0-2: `test_async_keepalive.c` 未注册到 CMakeLists.txt
- **状态**: [x] 已修复
- **方案**: 已注册至 `add_csilk_test`，CI 已覆盖。

---

## P1 — 高优先级

### P1-1: `write_chunk_frame` 超大数据静默丢弃
- **状态**: [x] 已修复
- **方案**: 支持分块写入，处理超过 `UINT_MAX` 的单次写入请求，避免静默丢失。

### P1-2: CORS 中间件缺少 `Vary: Origin`
- **状态**: [x] 已修复
- **方案**: 当 `Access-Control-Allow-Origin` 不为 `*` 时，自动设置 `Vary: Origin`。

### P1-3: `send_chunked_headers` 重复计算 strlen
- **状态**: [x] 已修复
- **方案**: 已复用 `h->key_len` 和 `h->value_len` 优化性能。

### P1-4: 无 OOM / malloc 失败测试
- **状态**: [x] 已修复
- **方案**: 引入了 `TEST_OOM` 宏及 `csilk_test.h` 模拟框架，新增 `tests/test_oom.c` 覆盖关键路径。

---

## P2 — 中优先级

### P2-1: `csilk_ctx_s` 内部结构暴露
- **状态**: [x] 已修复 (v0.3.0 演进)
- **方案**: 结构体已移至 `src/core/context_internal.h`，对外仅通过 `csilk_ctx_t` 不透明指针访问。新增了 `csilk_get_status`, `csilk_get_arena`, `csilk_set_response_body` 等一系列 Accessor 函数，彻底实现了 ABI 稳定性。同时通过 `$<BUILD_INTERFACE>` 允许内部开发及测试继续访问内部结构，兼顾了封装性与工程便利。

### P2-2: `csilk_redirect` 可改进
- **状态**: [x] 已修复
- **方案**: 移除了冗余的 body 分配，直接使用 status 控制。

### P2-3: `csilk_ctx_cleanup` 未重置 `on_ws_message`
- **状态**: [x] 已修复
- **方案**: 显式重置回调指针及 `request_id`。

### P2-4: Email 验证不完整
- **状态**: [x] 已修复
- **方案**: 增加了空格校验及 `@` 符号数量校验。

### P2-5: Session ID 使用 `rand()` 而非安全随机
- **状态**: [x] 已修复
- **方案**: 全面改用 `csilk_generate_uuid` (基于 `/dev/urandom`)。

### P2-6: `csilk_set` storage 无上限
- **状态**: [x] 已修复
- **方案**: 增加了 64 个存储项的硬上限，防止恶意请求内存耗尽。

---

## P3 — 低优先级 & 代码质量

### P3-1: Doxyfile `PROJECT_NUMBER` 未同步
- **状态**: [x] 已修复
- **位置**: `Doxyfile:3`

### P3-2: 重复 `typedef` `csilk_ctx_t`
- **状态**: [x] 已修复
- **位置**: `include/csilk.h`

### P3-3: Request ID / 追踪中间件缺失
- **状态**: [x] 已实现
- **方案**: 新增 `csilk_request_id_middleware` 自动生成追踪 ID 并写入 `X-Request-Id`。

### P3-7: JWT 认证中间件缺失
- **状态**: [x] 已实现 (v0.3.0)
- **方案**: 新增 `csilk_jwt_middleware` 支持 HS256 签名、过期校验及 Payload 自动存储，扩展了核心 Crypto 工具库（HMAC-SHA256, Base64URL）。

### P3-9: 缺少扩展 Hooks 与驱动接口
- **状态**: [x] 已实现 (v0.3.0)
- **方案**: 
  - **Hook 系统**: 实现了覆盖服务器启停、连接建立/关闭、请求开始/结束的生命周期 Hooks。
  - **Crypto 驱动**: 引入了 `csilk_crypto_driver_t` 接口，允许第三方无缝接入自定义的加密、哈希及 UUID 生成算法。

### P3-4: Health check 端点缺失
- **状态**: [x] 已实现
- **方案**: 新增 `csilk_health_check_handler` 提供 `/healthz` 支持。

### P3-5: `gzip.c` 未检查是否已压缩
- **状态**: [x] 已修复
- **方案**: 增加跳过逻辑，对已编码或媒体类 Content-Type (Image/Video/Zip) 不再压缩。

### P3-6: `test_config.c` 临时文件不清理
- **状态**: [x] 已修复
- **方案**: 增加 `unlink` 逻辑。
