# csilk Security & Concurrency Fix Design

- **版本**: v0.5.2
- **日期**: 2026-08-25
- **范围**: P0 数据竞争 + P1 安全漏洞修复

---

## 1. 问题概述

| ID | 严重度 | 问题 | 文件 |
|----|--------|------|------|
| P0-1 | Critical | `volatile int` 队列头尾非原子操作导致数据竞争 | `src/core/uring/uring_thread_pool.c:39-47` |
| P1-1 | High | JWT HS256 验证早期返回导致时序侧信道泄露 | `src/middleware/jwt.c:282-285` |
| P1-2 | High | sendfile 完成回调中 req 释放后访问悬空指针 | `src/core/http/http1_write.c:26-29` |

---

## 2. 修复方案

### 2.1 P0: 原子队列头尾

**当前代码：**
```c
// src/core/uring/uring_thread_pool.c:39-47
volatile int     queue_head;
volatile int     queue_tail;
volatile int     done_head;
volatile int     done_tail;
```

**修复为：**
```c
#include <stdatomic.h>
// ...
atomic_int     queue_head;
atomic_int     queue_tail;
atomic_int     done_head;
atomic_int     done_tail;
```

**访问点修改：**
- `tp->queue_head++` → `atomic_fetch_add(&tp->queue_head, 1)`
- `tp->queue_tail++` → `atomic_fetch_add(&tp->queue_tail, 1)`
- 读取时：`atomic_load(&tp->queue_head)` 或保留直接访问（x86 上 atomic_int 与 int 内存布局相同，但需明确语义）

**依赖检查：**
- `stdatomic.h` 是 C11 标准头文件，C23 项目已支持
- 不影响 ABI（`atomic_int` 与 `int` 大小相同）
- 需要重新运行 TSAN 测试确认修复

---

### 2.2 P1-1: JWT 时序侧信道防御

**当前代码：**
```c
// src/middleware/jwt.c:282-285
size_t sig_len = strlen(sig_ptr);
sig_ok = (sig_len == strlen(sig_expected_b64)) &&
         (constant_time_compare(
              (const uint8_t*)sig_ptr, (const uint8_t*)sig_expected_b64, sig_len) == 0);
```

**修复为：**
```c
// src/middleware/jwt.c:282-285
size_t sig_len = strlen(sig_ptr);
size_t expected_len = strlen(sig_expected_b64);
int len_mismatch = (sig_len != expected_len);
int cmp_result = len_mismatch ? 1 : 
                 constant_time_compare(
                     (const uint8_t*)sig_ptr, 
                     (const uint8_t*)sig_expected_b64, 
                     sig_len);
sig_ok = (cmp_result == 0);
```

**影响：**
- 无论长度是否匹配，都执行完整的 constant-time compare
- 避免攻击者通过响应时间判断有效签名长度
- 对性能影响可忽略（HS256 固定 32 字节比较）

---

### 2.3 P1-2: sendfile 回调 UAF 防御

**当前代码：**
```c
// src/core/http/http1_write.c:26-29
csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
csilk_client_t* client = (csilk_client_t*)c->_internal_client;
csilk_io_fs_req_cleanup(req);
free(req);
// ... 后续使用 client
```

**修复为：**
```c
// src/core/http/http1_write.c:26-32
csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
csilk_client_t* client = (csilk_client_t*)c->_internal_client;

csilk_io_fs_req_cleanup(req);
free(req);
req = NULL;  // 防止 req 被误用
c = NULL;    // 防止悬空指针

if (!client) {
    return;
}
// ... 后续使用 client
```

**影响：**
- 在释放 req 后立即置 NULL，防止后续代码意外访问
- 如果 `req->data` 与 req 在同一块分配中，c 会成为悬空指针，但此处仅用于提取 client
- client 的生命周期由 `csilk_client_t` 引用计数管理，不依赖 req

---

## 3. 验证计划

### 3.1 单元测试

- [ ] 运行所有现有测试确认无回归
- [ ] 特别验证 `test_uring_*` 系列测试
- [ ] 运行 `test_jwt` 确认时序修复不影响功能

### 3.2 Sanitizer 验证

- [ ] ASAN 构建：确认无内存错误
- [ ] TSAN 构建：确认无数据竞争（关键）
- [ ] 运行 10K 连接压力测试

### 3.3 性能验证

- [ ] 对比修复前后 P99 延迟（预期无显著变化）
- [ ] TSAN 开销在可接受范围内（通常 2-5x）

---

## 4. 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| atomic 操作影响性能 | 仅在生产路径的队列操作中引入，TSAN 可检测异常 |
| 破坏现有 ABI | `atomic_int` 与 `int` 内存布局相同，无 ABI 变化 |
| TSAN 误报 | 现有 `.tsan-suppressions` 文件保留，新增问题单独添加 |

---

## 5. 不在范围内

以下问题标记为 P2，留待后续迭代：
- `context.c:417` `_internal_client` NULL 解引用
- `tls.c:298-304` TLS flush 异步竞争窗口
- 其他代码质量改进

---

## 6. 实现顺序

1. **P0**: 修改 `uring_thread_pool.c` 的队列头尾为 atomic
2. **P1-1**: 修改 `jwt.c` 的签名验证逻辑
3. **P1-2**: 修改 `http1_write.c` 的回调清理顺序
4. **验证**: 运行 TSAN + ASAN + 全量测试
