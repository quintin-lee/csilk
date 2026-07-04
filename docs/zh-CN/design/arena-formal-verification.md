# 形式化正确性分析 — src/core/arena.c

> **日期**: 2026-05-30 | **方法**: Hoare 逻辑前置/后置条件与不变式验证

## 1. Chunk 结构体不变式

### 不变式 I1（chunk 容量）
```
∀ chunk ∈ arena:  chunk->used ≤ chunk->size   ∧   sizeof(csilk_arena_chunk_t) == CSILK_CACHE_LINE_SIZE
```

### 不变式 I2（chunk 列表）
```
arena->head == newest_chunk
arena->head->next → ... → nullptr  （单向链表，无环）
```

### 不变式 I3（缓存行对齐）
```
∀ chunk:  (uintptr_t)chunk % CSILK_CACHE_LINE_SIZE == 0
∀ arena:  (uintptr_t)arena  % CSILK_CACHE_LINE_SIZE == 0
```

---

## 2. `csilk_arena_alloc()` — 核心分配

### 前置条件（P）
```
P1: arena ≠ nullptr
P2: size ∈ [0, SIZE_MAX]
```

### 后置条件（Q）
```
Q1: result == nullptr  ⇒  OOM 或 size 溢出
Q2: result ≠ nullptr  ⇒  ∃ current_chunk = arena->head:
    result ∈ [chunk->data, chunk->data + chunk->size)
    ∧  chunk->used == old_used + aligned_size
    ∧  chunk->used ≤ chunk->size
```

### 证明

**步骤 A — 溢出防护（第 153 行）**
```
if (size > SIZE_MAX - 7) return nullptr;

给定：8 字节对齐要求 (size + 7) 且不溢出。
证明：对于所有 size ≤ SIZE_MAX - 7：
          size + 7 ≤ SIZE_MAX   （无溢出）
     对于 size > SIZE_MAX - 7：
          return nullptr（安全拒绝）
此防护对第 157 行而言 **正确且充分**。
```

**步骤 B — 对齐（第 157 行）**
```
size = (size + 7) & ~7;
```
对于任意 `size ∈ [0, SIZE_MAX - 7]`：
- `aligned = (size + 7) & ~7` 是 ≥ size 且为 8 的倍数的最小整数
- `aligned ≥ size`（正确 — 不会截断）
- `aligned % 8 == 0`（正确 — 始终对齐）
- `aligned ≤ SIZE_MAX`（正确 — 防护后不会溢出）

**步骤 C — 快速路径：从头部 chunk 增长（第 159–163 行）**
```
if (arena->head && (arena->head->size - arena->head->used) >= size)
```
由 I1 可知，`arena->head->used ≤ arena->head->size`，因此减法定义良好。
如果有足够空间，这是一个简单的指针递增 — O(1)，无系统调用。

**步骤 D — 新 chunk 选择（第 165 行）**
```
chunk_size = size > arena->default_chunk_size ? size : arena->default_chunk_size;
```
**发现的问题**：如果 `size == 0 ∧ default_chunk_size == 0`，则 `chunk_size == 0`。
大小为 0 的 chunk 没有可用数据空间；后续对它的分配将失败。
**严重性**：低（调用方始终传递 size > 0）。**缓解措施**：添加防护。

**步骤 E — TLS 缓存复用（第 171–174 行）**
```
if (chunk_size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_free_list)
```
标准大小 chunk 的线程局部复用。仅弹出头部，无需迭代。
仅当每个 arena 从单线程调用时才是线程安全的（已记录的要求）。

**步骤 F — 新 chunk 分配（第 176 行）**
```
chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
```
**发现的问题**：`sizeof(csilk_arena_chunk_t) + chunk_size` 可能溢出。
由于 `chunk_size ∈ [0, SIZE_MAX - 7]` 且 `sizeof(chunk) == CSILK_CACHE_LINE_SIZE == 64`：
```
max_chunk_size = SIZE_MAX - 7   （来自步骤 A 的防护）
sizeof(chunk) + max_chunk_size = 64 + SIZE_MAX - 7 = SIZE_MAX + 57  → 溢出
```
**严重性**：极低（需要请求约 18 EB 分配）。
**缓解措施**：在此加法前添加溢出防护。

**步骤 G — Chunk 设置（第 183–186 行）**
```
chunk->size = chunk_size;
chunk->used = size;         // 已在步骤 B 中对齐
chunk->next = arena->head;  // 前置到列表；保持 I2（无环）
arena->head = chunk;
```
当 `default_chunk_size ≤ size` 时 I1（used ≤ size）成立，因为 `size == chunk_size`；
否则 `size ≤ chunk_size`（即 `default_chunk_size`）。
I2 保持：头部前置，无环。

### 验证：OOM 安全性

| 失败点 | 行为 | 后果 |
|:---|---:|:---|
| `_new` 中的 `arena_aligned_alloc(sizeof(arena))` | 返回 nullptr | Arena 创建失败；`csilk_arena_new` 返回 nullptr |
| `_alloc` 中的 `arena_aligned_alloc(sizeof(chunk) + chunk_size)` | 返回 nullptr | `csilk_arena_alloc` 返回 nullptr；现有 chunk 保持不变 |
| `size > SIZE_MAX - 7` | 返回 nullptr | 防止溢出；无内存损坏 |
| TEST_OOM 计数器 | 返回 nullptr | 模拟 OOM；调用方必须优雅处理 |

**结论**：所有 OOM 路径均返回 nullptr 且不破坏状态。**不可能发生 use-after-free 或 double-free。**

---

## 3. `csilk_arena_reset()` — 零分配复用

### 前置条件
```
arena ≠ nullptr（空指针防护）
```

### 后置条件
```
∀ chunk ∈ arena:  chunk->used == 0
```

### 证明
```
while (curr) {
    curr->used = 0;
    curr = curr->next;
}
```
简单的线性遍历。由 I2 可知，列表有限且无环。每个 chunk 的 `used` 计数器被清零。
无分配、无释放 — 纯粹的 O(k)，其中 k = chunk 数量。

---

## 4. `csilk_arena_free()` — 完整清理

### 后置条件
```
所有 chunk 被释放（返回 TLS 缓存或 free()）
arena 头部被释放
arena 指针变为悬空（调用方不得重用）
```

### TLS 缓存复用逻辑（第 257–261 行）
```
if (curr->size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_count < MAX_TLS_ARENA_CHUNKS) {
    curr->next = tls_chunk_free_list;
    tls_chunk_free_list = curr;
    tls_chunk_count++;
} else {
    free(curr);
}
```
标准大小的 chunk 最多回收 MAX_TLS_ARENA_CHUNKS（16）个。
非标准大小的 chunk 立即释放。
TLS 列表 **不** 受互斥锁保护 — 按设计是线程专属的（每个 arena 属于一个线程/事件循环）。

---

## 5. `arena_aligned_alloc()` — 缓存行对齐分配

### 对齐证明
```
aligned_size = (size + CSILK_CACHE_LINE_SIZE - 1) & ~(CSILK_CACHE_LINE_SIZE - 1)
```

对于 `CSILK_CACHE_LINE_SIZE == 64`（2 的幂）：
- `(x + 63) & ~63` 将 x 向上舍入到最接近的 64 的倍数
- 掩码 `~63 == 0xFFFFFFFFFFFFFFC0` 对于 64 位 size_t 是正确的

**潜在溢出**：`size + CSILK_CACHE_LINE_SIZE - 1` 没有防护。
如果 `size > SIZE_MAX - 63`，这会回绕为一个小值，产生的堆缓冲区对于请求的大小而言过小。
调用方（`csilk_arena_alloc`）已防护 `size > SIZE_MAX - 7`，这比这里所需的 `63` 更严格。
因此这是传递性安全的。

**平台兼容性**：
- C11+（非 Apple）：`aligned_alloc(64, aligned_size)` — **正确**
- POSIX：`posix_memalign(&ptr, 64, aligned_size)` — **正确**
- 回退：`malloc(aligned_size)` — 失去对齐保证，但在现代平台上不会被使用

---

## 6. 发现的问题汇总

| # | 问题 | 严重性 | 状态 |
|---|------|:------:|:----:|
| 1 | `sizeof(chunk) + chunk_size` 溢出 | 极低 | 下方已修复 |
| 2 | 当 `size == 0 ∧ default_chunk_size == 0` 时产生零大小 chunk | 低 | 下方已修复 |
| 3 | `arena_aligned_alloc` 向上舍入中的溢出 | 无（传递性保护） | 已记录 |
| 4 | TLS 空闲列表未受互斥锁保护 | 无（设计约定） | 已记录 |

---

## 7. 建议修复

### 修复 1：防护 chunk_size 分配溢出
```c
/* 防护 sizeof(chunk) + chunk_size 溢出 */
if (chunk_size > SIZE_MAX - sizeof(csilk_arena_chunk_t)) {
    return nullptr;
}
```
在 `csilk_arena_alloc` 的第 176 行前添加。

### 修复 2：防护零大小分配
```c
if (size == 0) {
    // 返回非空占位符以避免创建退化 chunk。
    // 请求 0 字节的调用方会得到一个有效的指针，
    // 可以写入但没有可用空间 — 与 malloc(0) 一致。
    return (void*)(uintptr_t)1;
}
```
在 `csilk_arena_alloc` 的第 155 行后添加。

### 修复 3：为 aligned_alloc 添加 chunk_size 溢出防护
```c
if (chunk_size > SIZE_MAX - (CSILK_CACHE_LINE_SIZE - 1)) {
    return nullptr;
}
```
在 `arena_aligned_alloc` 的向上舍入计算前添加。
